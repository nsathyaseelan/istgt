#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <istgt_proto.h>
#include <sys/prctl.h>
#include <sys/eventfd.h>
#include "zrepl_prot.h"
#include "replication.h"
#include "istgt_integration.h"
#include "replication_misc.h"
#include "istgt_crc32c.h"
#include "istgt_misc.h"
#include "ring_mempool.h"
#include "istgt_scsi.h"

cstor_conn_ops_t cstor_ops = {
	.conn_listen = replication_listen,
	.conn_connect = replication_connect,
};

rte_smempool_t rcmd_mempool;
rte_smempool_t rcommon_cmd_mempool;
size_t rcmd_mempool_count = RCMD_MEMPOOL_ENTRIES;
size_t rcommon_cmd_mempool_count = RCOMMON_CMD_MEMPOOL_ENTRIES;

static void handle_mgmt_conn_error(replica_t *r, int sfd, struct epoll_event *events,
    int ev_count);
static int read_io_resp(spec_t *spec, replica_t *replica, io_event_t *revent, mgmt_cmd_t *mgmt_cmd);
static void respond_with_error_for_all_outstanding_mgmt_ios(replica_t *r);
static void inform_data_conn(replica_t *r);
static void free_replica(replica_t *r);
static int handle_mgmt_event_fd(replica_t *replica);

#define build_rcomm_cmd 						\
	do {								\
		rcomm_cmd = get_from_mempool(&rcommon_cmd_mempool);	\
		rcomm_cmd->copies_sent = 0;				\
		rcomm_cmd->total_len = 0;				\
		rcomm_cmd->offset = offset;				\
		rcomm_cmd->data_len = nbytes;				\
		rcomm_cmd->state = CMD_CREATED;				\
		rcomm_cmd->luworker_id = cmd->luworkerindx;		\
		rcomm_cmd->mutex = 					\
		    &spec->luworker_rmutex[cmd->luworkerindx];		\
		rcomm_cmd->cond_var = 					\
		    &spec->luworker_rcond[cmd->luworkerindx];		\
		rcomm_cmd->healthy_count = spec->healthy_rcount;	\
		rcomm_cmd->io_seq = ++spec->io_seq;			\
		rcomm_cmd->replication_factor = 			\
		    spec->replication_factor;				\
		rcomm_cmd->consistency_factor =				\
		    spec->consistency_factor;				\
		rcomm_cmd->state = CMD_ENQUEUED_TO_WAITQ;		\
		switch (cmd->cdb0) {					\
			case SBC_WRITE_6:				\
			case SBC_WRITE_10:				\
			case SBC_WRITE_12:				\
			case SBC_WRITE_16:				\
				cmd_write = true;			\
				break;					\
			default:					\
				break;					\
		}							\
		if (cmd_write) {						\
			rcomm_cmd->opcode = ZVOL_OPCODE_WRITE;		\
			rcomm_cmd->iovcnt = cmd->iobufindx+1;		\
		} else {						\
			rcomm_cmd->opcode = ZVOL_OPCODE_READ;		\
			rcomm_cmd->iovcnt = 0;				\
		}							\
		if (cmd_write) {						\
			for (i=1; i < iovcnt + 1; i++) {		\
				rcomm_cmd->iov[i].iov_base =		\
				    cmd->iobuf[i-1].iov_base;		\
				rcomm_cmd->iov[i].iov_len =		\
				    cmd->iobuf[i-1].iov_len;		\
			}						\
			rcomm_cmd->total_len += cmd->iobufsize;		\
		}							\
	} while (0)

#define BUILD_REPLICA_MGMT_HDR(_mgmtio_hdr, _mgmt_opcode, _data_len)	\
	do {								\
		_mgmtio_hdr = malloc(sizeof(zvol_io_hdr_t));		\
		_mgmtio_hdr->opcode = _mgmt_opcode;			\
		_mgmtio_hdr->version = REPLICA_VERSION;			\
		_mgmtio_hdr->len    = _data_len;			\
	} while (0)

#define	clear_mgmt_cmd(_replica, _mgmt_cmd)				\
	do {								\
		TAILQ_REMOVE(&_replica->mgmt_cmd_queue, _mgmt_cmd,	\
		    mgmt_cmd_next);					\
		if (_mgmt_cmd->io_hdr)					\
			free(_mgmt_cmd->io_hdr);			\
		if (_mgmt_cmd->data)					\
			free(_mgmt_cmd->data);				\
		free(_mgmt_cmd);					\
	} while(0)

/*
 * breaks if fd is closed or drained recv buf of fd
 * updates amount of data read to continue next time
 */
#define CHECK_AND_ADD_BREAK_IF_PARTIAL(_io_read, _count, _reqlen,	\
	    _donecount)							\
{									\
	if (_count == -1) {						\
		_donecount = -1;					\
		break;							\
	}								\
	if ((uint64_t) _count != _reqlen) {				\
		(_io_read) += _count;					\
		break;							\
	}								\
}

/*
 * Update_volstate enables spec and initialize io_seq number
 * if (n_replica >= c_factor)  or
 *    (n_replica >= MAX (c_factor, r_factor - c_factor + 1).
 *  n_replica = number of replica connected
 *  c_factor = consistency factor
 *  r_factor = replication_factor
 */
void
update_volstate(spec_t *spec)
{
	uint64_t max;
	replica_t *replica;

	if(((spec->healthy_rcount + spec->degraded_rcount >= spec->consistency_factor) &&
		(spec->healthy_rcount >= 1))||
		(spec->healthy_rcount  + spec->degraded_rcount 
			>= MAX(spec->replication_factor - spec->consistency_factor + 1, spec->consistency_factor))) {
		if (spec->ready == false)
		{
			max = 0;
			TAILQ_FOREACH(replica, &spec->rq, r_next)
				max = (max < replica->initial_checkpointed_io_seq) ?
				    replica->initial_checkpointed_io_seq : max;

			max = (max == 0) ? 10 : max + (1<<20);
			spec->io_seq = max;
		}
		spec->ready = true;
	} else {
		spec->ready = false;
	}
}

/*
 * perform read/write on fd for 'len' according to state
 * sets 'errorno' if read/write operation returns < 0
 * closes fd if operation returns < 0 && errno != EAGAIN|EWOULDBLOCK|EINTR,
 * and sets fd_closed also closes fd if read return 0, i.e., EOF
 * returns number of bytes read/written
 */
ssize_t
perform_read_write_on_fd(int fd, uint8_t *data, uint64_t len, int state)
{
	int64_t rc = -1;
	ssize_t nbytes = 0;
	int read_cmd = 0;

	while(1) {
		switch (state) {
			case READ_IO_RESP_HDR:
			case READ_IO_RESP_DATA:
				rc = read(fd, data + nbytes, len - nbytes);
				read_cmd = 1;
				break;

			case WRITE_IO_SEND_HDR:
			case WRITE_IO_SEND_DATA:
				rc = write(fd, data + nbytes, len - nbytes);
				break;

			default:
				REPLICA_ERRLOG("received invalid state(%d)\n", state);
				errno = EINVAL;
				break;
		}

		if(rc < 0) {
			if (errno == EINTR) {
				continue;
			} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return nbytes;
			} else {
				REPLICA_ERRLOG("received err %d on fd %d, closing it..\n", errno, fd);
				return -1;
			}
		} else if (rc == 0 && read_cmd) {
			REPLICA_ERRLOG("received EOF on fd %d, closing it..\n", fd);
			return -1;
		}

		nbytes += rc;
		if((size_t)nbytes == len) {
			break;
		}
	}

	return nbytes;
}

/*
 * get_all_read_resp_data_chunk will create/update io_data_chunk_list
 * with response received from replica according to io_numer.
 */
void
get_all_read_resp_data_chunk(replica_rcomm_resp_t *resp, struct io_data_chunk_list_t *io_chunk_list)
{
	zvol_io_hdr_t *hdr = &resp->io_resp_hdr;
	struct zvol_io_rw_hdr *io_hdr;
	uint8_t *dataptr = resp->data_ptr;
	uint64_t len = 0, parsed = 0;
	io_data_chunk_t  *io_chunk_entry;

	if (!TAILQ_EMPTY(io_chunk_list)) {
		io_chunk_entry = TAILQ_FIRST(io_chunk_list);

		while (parsed < hdr->len) {
			io_hdr = (struct zvol_io_rw_hdr *)dataptr;

			if (io_chunk_entry->io_num < io_hdr->io_num) {
				io_chunk_entry->data = dataptr + sizeof(struct zvol_io_rw_hdr) + len;
				io_chunk_entry->io_num = io_hdr->io_num;
				io_chunk_entry->len = io_hdr->len;
			}

			len += io_hdr->len;
			dataptr += (sizeof(struct zvol_io_rw_hdr) + io_hdr->len);
			parsed += (sizeof(struct zvol_io_rw_hdr) + io_hdr->len);
			io_chunk_entry = TAILQ_NEXT(io_chunk_entry, io_data_chunk_next);
		}
	} else {
		while (parsed < hdr->len) {
			io_hdr = (struct zvol_io_rw_hdr *)dataptr;

			io_chunk_entry = malloc(sizeof(io_data_chunk_t));
			io_chunk_entry->io_num = io_hdr->io_num;
			io_chunk_entry->data = dataptr + sizeof(struct zvol_io_rw_hdr);
			io_chunk_entry->len = io_hdr->len;
			TAILQ_INSERT_TAIL(io_chunk_list, io_chunk_entry, io_data_chunk_next);

			dataptr += (sizeof(struct zvol_io_rw_hdr) + io_hdr->len);
			parsed += (sizeof(struct zvol_io_rw_hdr) + io_hdr->len);
		}
	}
}

/*
 * process_chunk_read_resp forms a response data from io_data_chunk_list.
 */
uint8_t *
process_chunk_read_resp(struct io_data_chunk_list_t  *io_chunk_list, uint64_t len)
{
	uint64_t parsed = 0;
	uint8_t *read_data;
	io_data_chunk_t *io_chunk_entry, *io_chunk_next;

	read_data = xmalloc(len);

	io_chunk_entry = TAILQ_FIRST(io_chunk_list);
	while (io_chunk_entry) {
		memcpy(read_data + parsed, io_chunk_entry->data, io_chunk_entry->len);
		parsed += io_chunk_entry->len;

		io_chunk_next = TAILQ_NEXT(io_chunk_entry, io_data_chunk_next);
		TAILQ_REMOVE(io_chunk_list, io_chunk_entry, io_data_chunk_next);
		free(io_chunk_entry);
		io_chunk_entry = io_chunk_next;
	}
	return read_data;
}

/* creates replica entry and adds to spec's rwaitq list after creating mgmt connection */
replica_t *
create_replica_entry(spec_t *spec, int epfd, int mgmt_fd)
{
	replica_t *replica = NULL;
	int rc;

	replica = (replica_t *)malloc(sizeof(replica_t));
	if (!replica)
		return NULL;

	memset(replica, 0, sizeof(replica_t));
	replica->epfd = epfd;
	replica->mgmt_fd = mgmt_fd;

	TAILQ_INIT(&(replica->mgmt_cmd_queue));

	replica->initial_checkpointed_io_seq = 0;
	replica->mgmt_io_resp_hdr = malloc(sizeof(zvol_io_hdr_t));
	replica->mgmt_io_resp_data = NULL;

	MTX_LOCK(&spec->rq_mtx);
	TAILQ_INSERT_TAIL(&spec->rwaitq, replica, r_waitnext);
	MTX_UNLOCK(&spec->rq_mtx);

	replica->mgmt_eventfd2 = -1;
	replica->iofd = -1;
	replica->spec = spec;

	rc = pthread_mutex_init(&replica->r_mtx, NULL);
	if (rc != 0) {
		REPLICA_ERRLOG("pthread_mutex_init() failed errno:%d\n", errno);
		return NULL;
	}
	rc = pthread_cond_init(&replica->r_cond, NULL);
	if (rc != 0) {
		REPLICA_ERRLOG("pthread_cond_init() failed errno:%d\n", errno);
		return NULL;
	}
	return replica;
}

/*
 * update_replica_entry updates replica entry with IP/port,
 * perform handshake on data connection
 * starts replica thread to send/receive IOs to/from replica
 * removes replica from spec's rwaitq and adds it to spec's rq
 * Note: Locks in update_replica_entry are avoided since update_replica_entry is being
 *	 executed once only during handshake with replica.
 */
int
update_replica_entry(spec_t *spec, replica_t *replica, int iofd)
{
	int rc;
	zvol_io_hdr_t *rio_hdr;
	pthread_t r_thread;
	zvol_io_hdr_t *ack_hdr;
	mgmt_ack_t *ack_data;
	zvol_op_open_data_t *rio_payload;
	int i;

	ack_hdr = replica->mgmt_io_resp_hdr;	
	ack_data = (mgmt_ack_t *)replica->mgmt_io_resp_data;

	TAILQ_INIT(&replica->waitq);
	TAILQ_INIT(&replica->blockedq);
	TAILQ_INIT(&replica->readyq);

	replica->ongoing_io = NULL;
	replica->ongoing_io_len = 0;
	replica->ongoing_io_buf = NULL;
	replica->iofd = iofd;
	replica->ip = malloc(strlen(ack_data->ip)+1);
	strcpy(replica->ip, ack_data->ip);
	replica->port = ack_data->port;
	replica->state = ZVOL_STATUS_DEGRADED;
	replica->initial_checkpointed_io_seq = ack_hdr->checkpointed_io_seq;

	replica->pool_guid = ack_data->pool_guid;
	replica->zvol_guid = ack_data->zvol_guid;

	replica->spec = spec;
	replica->io_resp_hdr = (zvol_io_hdr_t *) malloc(sizeof (zvol_io_hdr_t));
	replica->io_state = READ_IO_RESP_HDR;
	replica->io_read = 0;

	rio_hdr = (zvol_io_hdr_t *) malloc(sizeof (zvol_io_hdr_t));
	rio_hdr->opcode = ZVOL_OPCODE_OPEN;
	rio_hdr->io_seq = 0;
	rio_hdr->offset = 0;
	rio_hdr->len = sizeof (zvol_op_open_data_t);
	rio_hdr->version = REPLICA_VERSION;

	rio_payload = (zvol_op_open_data_t *) malloc(
	    sizeof (zvol_op_open_data_t));
	rio_payload->timeout = (10 *60);
	rio_payload->tgt_block_size = spec->blocklen;
	strncpy(rio_payload->volname, spec->volname,
	    sizeof (rio_payload->volname));

	if (write(replica->iofd, rio_hdr, sizeof (*rio_hdr)) !=
	    sizeof (*rio_hdr)) {
		REPLICA_ERRLOG("failed to send io hdr to replica\n");
		goto replica_error;
	}

	if (write(replica->iofd, rio_payload, sizeof (zvol_op_open_data_t)) !=
	    sizeof (zvol_op_open_data_t)) {
		REPLICA_ERRLOG("failed to send data-open payload to replica\n");
		goto replica_error;
	}

	if (read(replica->iofd, rio_hdr, sizeof (*rio_hdr)) !=
	    sizeof (*rio_hdr)) {
		REPLICA_ERRLOG("failed to read data-open response from replica\n");
		goto replica_error;
	}

	if (rio_hdr->status != ZVOL_OP_STATUS_OK) {
		REPLICA_ERRLOG("data-open response is not OK\n");
		goto replica_error;
	}

	if (init_mempool(&replica->cmdq, rcmd_mempool_count, 0, 0,
	    "replica_cmd_mempool", NULL, NULL, NULL, false)) {
		REPLICA_ERRLOG("Failed to initialize replica cmdq\n");
		goto replica_error;
	}

	rc = make_socket_non_blocking(iofd);
	if (rc == -1) {
		REPLICA_ERRLOG("make_socket_non_blocking() failed errno:%d\n", errno);
		goto replica_error;
	}

	rc = pthread_create(&r_thread, NULL, &replica_thread,
			(void *)replica);
	if (rc != 0) {
		ISTGT_ERRLOG("pthread_create(r_thread) failed\n");
replica_error:
		replica->iofd = -1;
		close(iofd);
		free(rio_hdr);
		free(rio_payload);
		return -1;
	}

	free(rio_hdr);
	free(rio_payload);

	for (i = 0; (i < 10) && (replica->mgmt_eventfd2 == -1); i++)
		sleep(1);

	if (replica->mgmt_eventfd2 == -1) {
		ISTGT_ERRLOG("unable to set mgmteventfd2 for more than 10 seconds for replica %s %d..\n", replica->ip, replica->port);
		MTX_LOCK(&replica->r_mtx);
		replica->dont_free = 1;
		replica->iofd = -1;
		MTX_UNLOCK(&replica->r_mtx);
		close(iofd);
		return -1;
	}

	MTX_LOCK(&spec->rq_mtx);

	spec->replica_count++;
	spec->degraded_rcount++;
	TAILQ_REMOVE(&spec->rwaitq, replica, r_waitnext);
	TAILQ_INSERT_TAIL(&spec->rq, replica, r_next);
	update_volstate(spec);

	MTX_UNLOCK(&spec->rq_mtx);

	return 0;
}

/*
 * This function sends a handshake query to replica
 */
static int
send_replica_handshake_query(replica_t *replica, spec_t *spec)
{
	zvol_io_hdr_t *rmgmtio = NULL;
	size_t data_len = 0;
	uint8_t *data;
	zvol_op_code_t mgmt_opcode = ZVOL_OPCODE_HANDSHAKE;
	mgmt_cmd_t *mgmt_cmd;

	mgmt_cmd = malloc(sizeof(mgmt_cmd_t));

	data_len = strlen(spec->volname) + 1;

	BUILD_REPLICA_MGMT_HDR(rmgmtio, mgmt_opcode, data_len);

	data = (uint8_t *)malloc(data_len);
	snprintf((char *)data, data_len, "%s", spec->volname);

	mgmt_cmd->io_hdr = rmgmtio;
	mgmt_cmd->io_bytes = 0;
	mgmt_cmd->data = data;
	mgmt_cmd->mgmt_cmd_state = WRITE_IO_SEND_HDR;

	MTX_LOCK(&replica->r_mtx);
	TAILQ_INSERT_TAIL(&replica->mgmt_cmd_queue, mgmt_cmd, mgmt_cmd_next);
	MTX_UNLOCK(&replica->r_mtx);

	return 0;
}

/*
 * This function handles the response for SNAP_CREATE opcode.
 * In case of timeout, when the thread that triggered snapshot goes away,
 * this function handles deletion of rcommon_mgmt_cmd_t once all the responses
 * are received
 */
static void
handle_snap_create_resp(replica_t *replica, mgmt_cmd_t *mgmt_cmd)
{
	zvol_io_hdr_t *hdr = replica->mgmt_io_resp_hdr;
	rcommon_mgmt_cmd_t *rcomm_mgmt = mgmt_cmd->rcomm_mgmt;
	bool delete = false;
	MTX_LOCK(&rcomm_mgmt->mtx);
	if (hdr->status != ZVOL_OP_STATUS_OK)
		rcomm_mgmt->cmds_failed++;
	else
		rcomm_mgmt->cmds_succeeded++;
	if ((rcomm_mgmt->caller_gone == 1) &&
	    (rcomm_mgmt->cmds_sent == (rcomm_mgmt->cmds_failed + rcomm_mgmt->cmds_succeeded)))
		delete = true;
	MTX_UNLOCK(&rcomm_mgmt->mtx);
	if (delete == true)
		free(rcomm_mgmt);
}

static int
send_replica_snapshot(spec_t *spec, replica_t *replica, char *snapname, zvol_op_code_t opcode, rcommon_mgmt_cmd_t *rcomm_mgmt)
{
	zvol_io_hdr_t *rmgmtio = NULL;
	size_t data_len;
	char *data;
	zvol_op_code_t mgmt_opcode = opcode;
	mgmt_cmd_t *mgmt_cmd;
	uint64_t num = 1;
	int ret = 0;

	mgmt_cmd = malloc(sizeof(mgmt_cmd_t));
	mgmt_cmd->rcomm_mgmt = rcomm_mgmt;
	data_len = strlen(spec->volname) + strlen(snapname) + 2;

	BUILD_REPLICA_MGMT_HDR(rmgmtio, mgmt_opcode, data_len);

	data = (char *)malloc(data_len);
	snprintf(data, data_len, "%s@%s", spec->volname, snapname);

	mgmt_cmd->io_hdr = rmgmtio;
	mgmt_cmd->io_bytes = 0;
	mgmt_cmd->data = data;
	mgmt_cmd->mgmt_cmd_state = WRITE_IO_SEND_HDR;

	MTX_LOCK(&replica->r_mtx);
	//TODO: Add it as the second IO, rather than tailing
	TAILQ_INSERT_TAIL(&replica->mgmt_cmd_queue, mgmt_cmd, mgmt_cmd_next);
	MTX_UNLOCK(&replica->r_mtx);

	if (rcomm_mgmt != NULL)
		rcomm_mgmt->cmds_sent++;

	if (write(replica->mgmt_eventfd1, &num, sizeof (num)) != sizeof (num)) {
		REPLICA_NOTICELOG("Failed to inform to mgmt_eventfd for replica(%p)\n", replica);
		ret = -1;
	}

	return ret;
}

/*
static bool
any_ongoing_snapshot_command(spec_t *spec)
{
	replica_t *replica;
	mgmt_cmd_t *cmd;
	MTX_LOCK(&spec->rq_mtx);
	TAILQ_FOREACH(replica, &spec->rq, r_next) {
		MTX_LOCK(&replica->r_mtx);
		TAILQ_FOREACH(cmd, &replica->mgmt_cmd_queue, mgmt_cmd_next) {
			if (cmd->io_hdr->opcode == ZVOL_OPCODE_SNAP_CREATE) {
				MTX_UNLOCK(&replica->r_mtx);
				MTX_UNLOCK(&spec->rq_mtx);
				return true;
			}
		}
		MTX_UNLOCK(&replica->r_mtx);
	}
	MTX_UNLOCK(&spec->rq_mtx);
	return false;
}
*/

static int
is_volume_healthy(spec_t *spec)
{
	if (spec->healthy_rcount != spec->replication_factor)
		return false;
	return true;
}

/*
 * This function quiesces write IOs, waits for ongoing write IOs
 * If volume is not healthy or timeout happens while waiting for ongoing IOs,
 * write IOs will be allowed, and false will be returned.
 * If there are no pending write IOs and volume is healthy, true will be returned.
 * rq_mtx is required to be held by caller
 */
static bool
pause_and_timed_wait_for_ongoing_ios(spec_t *spec, int sec)
{
	struct timespec last, now, diff;
	bool ret = false;
	bool write_io_found = false;
	replica_t *replica;

	spec->quiesce = 1;

	clock_gettime(CLOCK_MONOTONIC, &last);
	timesdiff(last, now, diff);

	while ((diff.tv_sec < sec) && (is_volume_healthy(spec) == true)) {
		write_io_found = false;
		TAILQ_FOREACH(replica, &spec->rq, r_next) {
			if (replica->replica_inflight_write_io_cnt != 0) {
				write_io_found = true;
				break;
			}
		}
		if (write_io_found == false) {
			if (spec->inflight_write_io_cnt != 0) {
				write_io_found = true;
				break;
			}
		}
		if (write_io_found == false) {
			ret = true;
			break;
		}
		MTX_UNLOCK(&spec->rq_mtx);
		sleep (1);
		MTX_LOCK(&spec->rq_mtx);
		timesdiff(last, now, diff);
	}

	if (ret == false)
		spec->quiesce = 0;

	return ret;
}

int istgt_lu_destroy_snapshot(spec_t *spec, char *snapname)
{
	replica_t *replica;
	TAILQ_FOREACH(replica, &spec->rq, r_next)
		send_replica_snapshot(spec, replica, snapname, ZVOL_OPCODE_SNAP_DESTROY, NULL);
	return true;
}

/*
 * This API will create snapshot with given name on the spec.
 * It will wait for io_wait_time seconds to complete ongoing IOs.
 * Overall, this API will wait for wait_time seconds to get response
 * for snapshot command (this includes io_wait_time).
 * In case of any failures, snapshot destroy command will be sent to all replicas.
 */
int istgt_lu_create_snapshot(spec_t *spec, char *snapname, int io_wait_time, int wait_time)
{
	bool r;
	replica_t *replica;
	int free_rcomm_mgmt = 0;
	struct timespec last, now, diff;
	struct rcommon_mgmt_cmd *rcomm_mgmt;
	int rc;

	clock_gettime(CLOCK_MONOTONIC, &last);
	MTX_LOCK(&spec->rq_mtx);

	if (is_volume_healthy(spec) == false) {
		MTX_UNLOCK(&spec->rq_mtx);
		return false;
	}

	r = pause_and_timed_wait_for_ongoing_ios(spec, io_wait_time);
	if (r == false) {
		MTX_UNLOCK(&spec->rq_mtx);
		return false;
	}

	rcomm_mgmt = (struct rcommon_mgmt_cmd *)malloc(sizeof (struct rcommon_mgmt_cmd));
	pthread_mutex_init(&rcomm_mgmt->mtx, NULL);
	rcomm_mgmt->cmds_sent = 0;
	rcomm_mgmt->cmds_succeeded = 0;
	rcomm_mgmt->cmds_failed = 0;
	rcomm_mgmt->caller_gone = 0;
	free_rcomm_mgmt = 0;

	r = false;
	TAILQ_FOREACH(replica, &spec->rq, r_next) {
		rc = send_replica_snapshot(spec, replica, snapname, ZVOL_OPCODE_SNAP_CREATE, rcomm_mgmt);
		if (rc < 0) {
			rcomm_mgmt->caller_gone = 1;
			goto done;
		}
	}

	timesdiff(last, now, diff);
	MTX_LOCK(&rcomm_mgmt->mtx);

	if (rcomm_mgmt->cmds_sent != spec->replication_factor) {
		rcomm_mgmt->caller_gone = 1;
		MTX_UNLOCK(&rcomm_mgmt->mtx);
		goto done;
	}

	while (diff.tv_sec < wait_time) {
		if (rcomm_mgmt->cmds_sent == (rcomm_mgmt->cmds_succeeded + rcomm_mgmt->cmds_failed))
			break;
		MTX_UNLOCK(&rcomm_mgmt->mtx);
		MTX_UNLOCK(&spec->rq_mtx);
		sleep(1);
		MTX_LOCK(&spec->rq_mtx);
		MTX_LOCK(&rcomm_mgmt->mtx);
		timesdiff(last, now, diff);
	}
	rcomm_mgmt->caller_gone = 1;
	if (rcomm_mgmt->cmds_sent == (rcomm_mgmt->cmds_succeeded + rcomm_mgmt->cmds_failed)) {
		free_rcomm_mgmt = 1;
		if ((rcomm_mgmt->cmds_succeeded == spec->replication_factor) && (rcomm_mgmt->cmds_failed == 0))
			r = true;
	}
	MTX_UNLOCK(&rcomm_mgmt->mtx);
done:
	spec->quiesce = 0;
	if (r == false)
		TAILQ_FOREACH(replica, &spec->rq, r_next)
			send_replica_snapshot(spec, replica, snapname, ZVOL_OPCODE_SNAP_DESTROY, NULL);
	MTX_UNLOCK(&spec->rq_mtx);
	if (free_rcomm_mgmt == 1)
		free(rcomm_mgmt);
	return r;
}

/*
 * This function sends status query for a volume to replica
 */
static int
send_replica_status_query(replica_t *replica, spec_t *spec)
{
	zvol_io_hdr_t *rmgmtio = NULL;
	size_t data_len;
	uint8_t *data;
	zvol_op_code_t mgmt_opcode = ZVOL_OPCODE_REPLICA_STATUS;
	mgmt_cmd_t *mgmt_cmd;

	mgmt_cmd = malloc(sizeof(mgmt_cmd_t));
	data_len = strlen(spec->volname) + 1;
	BUILD_REPLICA_MGMT_HDR(rmgmtio, mgmt_opcode, data_len);

	data = (uint8_t *)malloc(data_len);
	snprintf((char *)data, data_len, "%s", spec->volname);

	mgmt_cmd->io_hdr = rmgmtio;
	mgmt_cmd->io_bytes = 0;
	mgmt_cmd->data = data;
	mgmt_cmd->mgmt_cmd_state = WRITE_IO_SEND_HDR;

	MTX_LOCK(&replica->r_mtx);
	TAILQ_INSERT_TAIL(&replica->mgmt_cmd_queue, mgmt_cmd, mgmt_cmd_next);
	MTX_UNLOCK(&replica->r_mtx);

	return handle_write_data_event(replica);
}

/*
 * ask_replica_status will send replica_status query to all degraded replica
 */
static void
ask_replica_status_all(spec_t *spec)
{
	int ret;
	replica_t *replica;

	MTX_LOCK(&spec->rq_mtx);
	TAILQ_FOREACH(replica, &spec->rq, r_next) {
		if (replica->state == ZVOL_STATUS_HEALTHY) {
			continue;
		}

		ret = send_replica_status_query(replica, spec);
		if (ret == -1) {
			REPLICA_ERRLOG("send mgmtIO for status failed on "
			    "replica(%s:%d) .. stopped sendign status "
			    "in this iteration\n", replica->ip, replica->port);
			MTX_UNLOCK(&spec->rq_mtx);
			handle_mgmt_conn_error(replica, 0, NULL, 0);
			return;
		}
	}
	MTX_UNLOCK(&spec->rq_mtx);
}

/*
 * This function processes the status response received from replica
 * and update spec's healthy/degrade count
 */
static int
update_replica_status(spec_t *spec, replica_t *replica)
{
	zrepl_status_ack_t *repl_status;
	replica_state_t last_status;

	repl_status = (zrepl_status_ack_t *)replica->mgmt_io_resp_data;

	MTX_LOCK(&spec->rq_mtx);
	MTX_LOCK(&replica->r_mtx);
	last_status = replica->state;
	replica->state = (replica_state_t) repl_status->state;
	MTX_UNLOCK(&replica->r_mtx);

	if(last_status != repl_status->state) {
		if (repl_status->state == ZVOL_STATUS_DEGRADED) {
			spec->degraded_rcount++;
			spec->healthy_rcount--;
		} else if (repl_status->state == ZVOL_STATUS_HEALTHY) {
			spec->degraded_rcount--;
			spec->healthy_rcount++;
		}
		update_volstate(spec);
	}
	MTX_UNLOCK(&spec->rq_mtx);
	return 0;
}

/*
 * forms data connection to replica, updates replica entry
 */
int
zvol_handshake(spec_t *spec, replica_t *replica)
{
	int rc, iofd;
	zvol_io_hdr_t *ack_hdr;
	mgmt_ack_t *ack_data;

	ack_hdr = replica->mgmt_io_resp_hdr;	
	ack_data = (mgmt_ack_t *)replica->mgmt_io_resp_data;

	if (ack_hdr->status != ZVOL_OP_STATUS_OK) {
		REPLICA_ERRLOG("mgmt_ack status is not ok..\n");
		return -1;
	}

	if(strcmp(ack_data->volname, spec->volname) != 0) {
		REPLICA_ERRLOG("volname %s not matching with spec %s volname\n",
		    ack_data->volname, spec->volname);
		return -1;
	}

	if((iofd = cstor_ops.conn_connect(ack_data->ip, ack_data->port)) < 0) {
		REPLICA_ERRLOG("conn_connect() failed errno:%d\n", errno);
		return -1;
	}

	rc = update_replica_entry(spec, replica, iofd);

	return rc;
}

/*
 * accepts (mgmt) connections on which handshake and other management IOs are sent
 * sends handshake IO to start handshake on accepted (mgmt) connection
 */
void
accept_mgmt_conns(int epfd, int sfd)
{
	struct epoll_event event;
	int rc, rcount=0;
	spec_t *spec;
	char *buf = malloc(BUFSIZE);
	int mgmt_fd;
	mgmt_event_t *mevent1, *mevent2;

	while (1) {
		struct sockaddr saddr;
		socklen_t slen;
		char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
		replica_t *replica = NULL;

		slen = sizeof(saddr);
		mgmt_fd = accept(sfd, &saddr, &slen);
		if (mgmt_fd == -1) {
			if((errno != EAGAIN) && (errno != EWOULDBLOCK))
				REPLICA_ERRLOG("accept() failed on fd %d, errno:%d.. better to restart listener..", sfd, errno);
			break;
		}

		rc = getnameinfo(&saddr, slen,
				hbuf, sizeof(hbuf),
				sbuf, sizeof(sbuf),
				NI_NUMERICHOST | NI_NUMERICSERV);
		if (rc == 0) {
			rcount++;
			REPLICA_LOG("Accepted connection on descriptor %d "
					"(host=%s, port=%s)\n", mgmt_fd, hbuf, sbuf);
		}
		rc = make_socket_non_blocking(mgmt_fd);
		if (rc == -1) {
			REPLICA_ERRLOG("make_socket_non_blocking() failed on fd %d, errno:%d.. closing it..", mgmt_fd, errno);
			close(mgmt_fd);
			continue;
		}

                MTX_LOCK(&specq_mtx);
                TAILQ_FOREACH(spec, &spec_q, spec_next) {
			// Since we are supporting single spec per controller
			// we will continue using first spec only	
                        break;
                }
                MTX_UNLOCK(&specq_mtx);

		/*
		 * As of now, we are supporting single spec_t per target
		 * So, we can assign spec to replica here.
		 * TODO: In case of multiple spec, asignment of spec to replica
		 * 	 should be handled in update_replica_entry func according to
		 * 	 volume name provided by replica.
		 */
		replica = create_replica_entry(spec, epfd, mgmt_fd);
		if (!replica) {
			REPLICA_ERRLOG("Failed to create replica for fd %dclosing it..", mgmt_fd);
			close(mgmt_fd);
			continue;
		}

		mevent1 = (mgmt_event_t *)malloc(sizeof(mgmt_event_t));
		mevent2 = (mgmt_event_t *)malloc(sizeof(mgmt_event_t));

		replica->mgmt_eventfd1 = eventfd(0, EFD_NONBLOCK);
		if (replica->mgmt_eventfd1 < 0) {
			REPLICA_ERRLOG("error for replica(%s:%d) mgmt_eventfd(%d) err(%d)\n",
			    replica->ip, replica->port, replica->mgmt_eventfd1, errno);
			goto cleanup;
		}

		mevent1->fd = replica->mgmt_eventfd1;
		mevent1->r_ptr = replica;
		event.data.ptr = mevent1;
		event.events = EPOLLIN;
		rc = epoll_ctl(epfd, EPOLL_CTL_ADD, replica->mgmt_eventfd1, &event);
		if(rc == -1) {
			REPLICA_ERRLOG("epoll_ctl() failed on fd %d, errno:%d.. closing it..", mgmt_fd, errno);
			goto cleanup;
		}

		mevent2->fd = mgmt_fd;
		mevent2->r_ptr = replica;
		event.data.ptr = mevent2;
		event.events = EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLET | EPOLLOUT | EPOLLRDHUP;

		rc = epoll_ctl(epfd, EPOLL_CTL_ADD, mgmt_fd, &event);
		if(rc == -1) {
			REPLICA_ERRLOG("epoll_ctl() failed on fd %d, errno:%d.. closing it..", mgmt_fd, errno);
cleanup:
			if (replica->mgmt_eventfd1 != -1) {
				epoll_ctl(epfd, EPOLL_CTL_DEL, replica->mgmt_eventfd1, NULL);
				close(replica->mgmt_eventfd1);
				free(mevent2);
				free(mevent1);
			}
			if (replica) {
				pthread_mutex_destroy(&replica->r_mtx);
				pthread_cond_destroy(&replica->r_cond);
				MTX_LOCK(&spec->rq_mtx);
				TAILQ_REMOVE(&spec->rwaitq, replica, r_waitnext);
				MTX_UNLOCK(&spec->rq_mtx);
			}
			close(mgmt_fd);
			continue;
		}
		replica->m_event1 = mevent1;
		replica->m_event2 = mevent2;
		send_replica_handshake_query(replica, spec);
	}
	free(buf);
}

/*
 * write_io_data will write IOs to mgmt connection only..
 * initial state is write_io_send_hdr, which will write header.
 * it transitions to write_io_send_data based on length in hdr.
 * once data is written, it will change io_state to READ_IO_RESP_HDR
 * to read data from replica.
 */
static int
write_io_data(replica_t *replica, io_event_t *wevent)
{
	int fd = wevent->fd;
	int *state = wevent->state;
	zvol_io_hdr_t *write_hdr = wevent->io_hdr;
	void **data = wevent->io_data;
	int *write_count = wevent->byte_count;
	uint64_t reqlen;
	ssize_t count;
	int donecount = 0;
	(void)replica;

	switch(*state) {
		case WRITE_IO_SEND_HDR:
			reqlen = sizeof (zvol_io_hdr_t) - (*write_count);
			count = perform_read_write_on_fd(fd,
			    ((uint8_t *)write_hdr) + (*write_count), reqlen, *state);
			CHECK_AND_ADD_BREAK_IF_PARTIAL((*write_count), count, reqlen, donecount);

			*write_count = 0;
			*state = WRITE_IO_SEND_DATA;

		case WRITE_IO_SEND_DATA:
			reqlen = write_hdr->len - (*write_count);
			if (reqlen != 0) {
				count = perform_read_write_on_fd(fd,
				    ((uint8_t *)(*data)) + (*write_count), reqlen, *state);
				CHECK_AND_ADD_BREAK_IF_PARTIAL((*write_count), count, reqlen, donecount);
			}
			free(*data);
			*data = NULL;
			*write_count = 0;
			donecount++;
			*state = READ_IO_RESP_HDR;
			break;
	}
	return donecount;
}


/*
 * initial state is read io_resp_hdr, which reads IO response.
 * it transitions to read io_resp_data based on length in hdr.
 * once data is handled, it goes to read hdr which can be new response.
 * this goes on until EAGAIN or connection gets closed.
 */
static int
read_io_resp(spec_t *spec, replica_t *replica, io_event_t *revent, mgmt_cmd_t *mgmt_cmd)
{
	int fd = revent->fd;
	int *state = revent->state;
	zvol_io_hdr_t *resp_hdr = revent->io_hdr;
	void **resp_data = revent->io_data;
	int *read_count = revent->byte_count;
	uint64_t reqlen;
	ssize_t count;
	int rc = 0;
	int donecount = 0;

	switch(*state) {
		case READ_IO_RESP_HDR:
read_io_resp_hdr:
			reqlen = sizeof (zvol_io_hdr_t) - (*read_count);
			count = perform_read_write_on_fd(fd,
			    ((uint8_t *)resp_hdr) + (*read_count), reqlen, *state);
			CHECK_AND_ADD_BREAK_IF_PARTIAL((*read_count), count, reqlen, donecount);

			*read_count = 0;
			if (resp_hdr->opcode == ZVOL_OPCODE_WRITE)
				resp_hdr->len = 0;
			if (resp_hdr->len != 0) {
				(*resp_data) = malloc(resp_hdr->len);
			}
			*state = READ_IO_RESP_DATA;

		case READ_IO_RESP_DATA:
			reqlen = resp_hdr->len - (*read_count);
			if (reqlen != 0) {
				count = perform_read_write_on_fd(fd,
				    ((uint8_t *)(*resp_data)) + (*read_count), reqlen, *state);
				CHECK_AND_ADD_BREAK_IF_PARTIAL((*read_count), count, reqlen, donecount);
			}

			*read_count = 0;

			switch (resp_hdr->opcode) {
				case ZVOL_OPCODE_HANDSHAKE:
					if(resp_hdr->len != sizeof (mgmt_ack_t))
						REPLICA_ERRLOG("mgmt_ack_len %lu not matching with size of mgmt_ack_data..\n",
						    resp_hdr->len);

					/* dont process handshake on data connection */
					if (fd != replica->iofd)
						rc = zvol_handshake(spec, replica);

					memset(resp_hdr, 0, sizeof(zvol_io_hdr_t));
					free(*resp_data);

					if (rc == -1)
						donecount = -1;
					break;

				case ZVOL_OPCODE_REPLICA_STATUS:
					if(resp_hdr->len != sizeof (zrepl_status_ack_t))
						REPLICA_ERRLOG("replica_state_t length %lu is not matching with size of repl status data..\n",
							resp_hdr->len);

					/* replica status must come from mgmt connection */
					if (fd != replica->iofd)
						update_replica_status(spec, replica);
					free(*resp_data);
					break;

				case ZVOL_OPCODE_SNAP_CREATE:
					/* snap create response must come from mgmt connection */
					handle_snap_create_resp(replica, mgmt_cmd);
					break;

				case ZVOL_OPCODE_SNAP_DESTROY:
					break;

				default:
					REPLICA_NOTICELOG("unsupported opcode(%d) received..\n", resp_hdr->opcode);
					break;
			}
			*resp_data = NULL;
			*read_count = 0;
			donecount++;
			*state = READ_IO_RESP_HDR;
			goto read_io_resp_hdr;
	}

	return donecount;
}

/*
 * This function send a command to replica from replica's
 * management command queue.
 */
int
handle_write_data_event(replica_t *replica)
{
	io_event_t wevent;
	int rc = 0;
	mgmt_cmd_t *mgmt_cmd = NULL;

	MTX_LOCK(&replica->r_mtx);
	mgmt_cmd = TAILQ_FIRST(&replica->mgmt_cmd_queue);
	if (!mgmt_cmd) {
		rc = 0;
		MTX_UNLOCK(&replica->r_mtx);
		return rc;
	}

	if (mgmt_cmd->mgmt_cmd_state != WRITE_IO_SEND_HDR &&
		mgmt_cmd->mgmt_cmd_state != WRITE_IO_SEND_DATA) {
		MTX_UNLOCK(&replica->r_mtx);
		REPLICA_ERRLOG("write IO is in wait state on mgmt connection..");
		return rc;
	}

	MTX_UNLOCK(&replica->r_mtx);

	wevent.fd = replica->mgmt_fd;
	wevent.state = &(mgmt_cmd->mgmt_cmd_state);
	wevent.io_hdr = mgmt_cmd->io_hdr;
	wevent.io_data = (void **)(&(mgmt_cmd->data));
	wevent.byte_count = &(mgmt_cmd->io_bytes);

	rc = write_io_data(replica, &wevent);
	return rc;
}

/*
 * This function will inform replica_thread(data_connection)
 * regarding error in replica's management connection.
 */
static void
inform_data_conn(replica_t *r)
{
	uint64_t num = 1;
	r->disconnect_conn = 1;
	if (write(r->mgmt_eventfd2, &num, sizeof (num)) != sizeof (num))
		REPLICA_NOTICELOG("Failed to inform err to data_conn for replica(%p)\n", r);
}

/*
 * This function will cleanup replica structure
 */
static void
free_replica(replica_t *r)
{
	pthread_mutex_destroy(&r->r_mtx);
	pthread_cond_destroy(&r->r_cond);

	free(r->mgmt_io_resp_hdr);
	free(r->m_event1);
	free(r->m_event2);

	if (r->ip)
		free(r->ip);
	free(r);
}

/*
 * This function will remove fd from epoll and close it.
 * epollfd - epoll fd
 * fd - fd needs to be closed
 */
void
close_fd(int epollfd, int fd)
{
	if (epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL) == -1) {
		REPLICA_ERRLOG("epoll error for fd(%d) err(%d)\n", fd, errno);
		return;
	}
	close(fd);
}

/*
 * This function empties mgmt queue, and
 * calls the callback with mgmt cmd status as failed
 */
static void
empty_mgmt_q_of_replica(replica_t *r)
{
	mgmt_cmd_t *mgmt_cmd;
	while ((mgmt_cmd = TAILQ_FIRST(&r->mgmt_cmd_queue))) {
		switch (mgmt_cmd->io_hdr->opcode) {
			case ZVOL_OPCODE_SNAP_CREATE:
				mgmt_cmd->io_hdr->status = ZVOL_OP_STATUS_FAILED;
				handle_snap_create_resp(r, mgmt_cmd);
				break;
			default:
				break;
		}
		clear_mgmt_cmd(r, mgmt_cmd);
	}
}

/*
 * This function will cleanup replica's management command queue
 */
static void
respond_with_error_for_all_outstanding_mgmt_ios(replica_t *r)
{
	empty_mgmt_q_of_replica(r);
}

#define build_rcmd() 							\
	do {								\
		uint8_t *ldata = malloc(sizeof(zvol_io_hdr_t) + 	\
		    sizeof(struct zvol_io_rw_hdr));			\
		zvol_io_hdr_t *rio = (zvol_io_hdr_t *)ldata;		\
		struct zvol_io_rw_hdr *rio_rw_hdr =			\
		    (struct zvol_io_rw_hdr *)(ldata +			\
		    sizeof(zvol_io_hdr_t));				\
		rcmd = get_from_mempool(&rcmd_mempool);			\
		memset(rcmd, 0, sizeof(*rcmd));				\
		rcmd->opcode = rcomm_cmd->opcode;			\
		rcmd->offset = rcomm_cmd->offset;			\
		rcmd->data_len = rcomm_cmd->data_len;			\
		rcmd->io_seq = rcomm_cmd->io_seq;			\
		rcmd->idx = rcomm_cmd->copies_sent - 1;			\
		rcmd->healthy_count = spec->healthy_rcount;		\
		rcmd->rcommq_ptr = rcomm_cmd;				\
		rcmd->status = 0;					\
		rcmd->iovcnt = rcomm_cmd->iovcnt;			\
		for (i=1; i < rcomm_cmd->iovcnt + 1; i++) {		\
			rcmd->iov[i].iov_base = 			\
			     rcomm_cmd->iov[i].iov_base;		\
			rcmd->iov[i].iov_len = 				\
			    rcomm_cmd->iov[i].iov_len;			\
		}							\
		rio->opcode = rcmd->opcode;				\
		rio->version = REPLICA_VERSION;				\
		rio->io_seq = rcmd->io_seq;				\
		rio->offset = rcmd->offset;				\
		if (rcmd->opcode == ZVOL_OPCODE_WRITE) {		\
			rio->len = rcmd->data_len +			\
			    sizeof(struct zvol_io_rw_hdr);		\
			rio->checkpointed_io_seq = 0;			\
		} else							\
			rio->len = rcmd->data_len;			\
		rcmd->iov_data = ldata;					\
		rio_rw_hdr->io_num = rcmd->io_seq;			\
		rio_rw_hdr->len = rcmd->data_len;			\
		rcmd->iov[0].iov_base = rio;				\
		if (rcomm_cmd->opcode == ZVOL_OPCODE_WRITE)		\
			rcmd->iov[0].iov_len = sizeof(zvol_io_hdr_t) +	\
			    sizeof(struct zvol_io_rw_hdr);		\
		else							\
			rcmd->iov[0].iov_len = sizeof(zvol_io_hdr_t);	\
		rcmd->iovcnt++;						\
	} while (0);							\

void
clear_rcomm_cmd(rcommon_cmd_t *rcomm_cmd)
{
	int i;
	for (i=1; i<rcomm_cmd->iovcnt + 1; i++)
		xfree(rcomm_cmd->iov[i].iov_base);
	put_to_mempool(&rcommon_cmd_mempool, rcomm_cmd);
}

static uint8_t *
handle_read_consistency(rcommon_cmd_t *rcomm_cmd)
{
	int i;
	uint8_t *dataptr = NULL;
	struct io_data_chunk_list_t io_data_chunk_list;

	TAILQ_INIT(&(io_data_chunk_list));

	for (i = 0; i < rcomm_cmd->copies_sent; i++) {
		if (rcomm_cmd->resp_list[i].status == 1) {
			get_all_read_resp_data_chunk(&rcomm_cmd->resp_list[i], &io_data_chunk_list);
		}
	}

	dataptr = process_chunk_read_resp(&io_data_chunk_list, rcomm_cmd->data_len);

	return dataptr;
}

static int
check_for_command_completion(spec_t *spec, rcommon_cmd_t *rcomm_cmd, ISTGT_LU_CMD_Ptr cmd)
{
	int i, rc = 0;
	uint8_t *data = NULL;
	int success = 0, failure = 0;

	for (i = 0; i < rcomm_cmd->copies_sent; i++) {
		if (rcomm_cmd->resp_list[i].status == 1) {
			success++;
		} else if (rcomm_cmd->resp_list[i].status == -1) {
			failure++;
		}
	}

	if (rcomm_cmd->opcode == ZVOL_OPCODE_READ) {
		if ((rcomm_cmd->copies_sent != (success + failure))) {
			rc = 0;
		} else if (rcomm_cmd->copies_sent == failure) {
			rc = -1;
		} else {
			data = handle_read_consistency(rcomm_cmd);
			cmd->data = data;
			rc = 1;
		}
	} else if (rcomm_cmd->opcode == ZVOL_OPCODE_WRITE) {
		if (success >= rcomm_cmd->consistency_factor) {
			rc = 1;
		} else if ((success + failure) == rcomm_cmd->copies_sent) {
			rc = -1;
		}

	}

	return rc;
}

int64_t
replicate(ISTGT_LU_DISK *spec, ISTGT_LU_CMD_Ptr cmd, uint64_t offset, uint64_t nbytes)
{
	int rc = -1, i;
	bool cmd_write = false;
	replica_t *replica;
	rcommon_cmd_t *rcomm_cmd;
	rcmd_t *rcmd = NULL;
	int iovcnt = cmd->iobufindx + 1;
	bool cmd_sent = false;
	struct timespec abstime, now;
	int nsec, err_num = 0;

	MTX_LOCK(&spec->rq_mtx);
	if(spec->ready == false) {
		REPLICA_LOG("SPEC is not ready\n");
		MTX_UNLOCK(&spec->rq_mtx);
		return -1;
	}

	build_rcomm_cmd;

	TAILQ_FOREACH(replica, &spec->rq, r_next) {
		/*
		 * If there are some healthy replica then send read command
		 * to all healthy replica else send read command to all
		 * degraded replica.
		 */
		if (spec->healthy_rcount &&
		    rcomm_cmd->opcode == ZVOL_OPCODE_READ) {
			/*
			 * If there are some healthy replica then don't send
			 * a read command to degraded replica
			 */
			if (replica->state == ZVOL_STATUS_DEGRADED)
				continue;
			else
				cmd_sent = true;
		}

		rcomm_cmd->copies_sent++;
		build_rcmd();
		put_to_mempool(&replica->cmdq, rcmd);
		eventfd_write(replica->data_eventfd, 1);

		if (cmd_sent)
			break;
	}

	TAILQ_INSERT_TAIL(&spec->rcommon_waitq, rcomm_cmd, wait_cmd_next);

	MTX_UNLOCK(&spec->rq_mtx);

	// now wait for command to complete
	while (1) {
		MTX_LOCK(rcomm_cmd->mutex);
		// check for status of rcomm_cmd
		rc = check_for_command_completion(spec, rcomm_cmd, cmd);
		if (rc) {
			if (rc == 1)
				rc = cmd->data_len = rcomm_cmd->data_len;
			rcomm_cmd->state = CMD_EXECUTION_DONE;
			put_to_mempool(&spec->rcommon_deadlist, rcomm_cmd);
			MTX_UNLOCK(rcomm_cmd->mutex);

			/*
			 * NOTE: This is for debugging purpose only
			 */
			if (err_num == ETIMEDOUT)
				fprintf(stderr,"last errno(%d) opcode(%d)\n",
				    errno, rcomm_cmd->opcode);

			MTX_LOCK(&spec->rq_mtx);
			TAILQ_REMOVE(&spec->rcommon_waitq, rcomm_cmd, wait_cmd_next);
			MTX_UNLOCK(&spec->rq_mtx);

			break;
		}

		/* wait for 500 ms(500000000 ns) */
		clock_gettime(CLOCK_REALTIME, &now);
		nsec = 1000000000 - now.tv_nsec;
		if (nsec > 500000000) {
			abstime.tv_sec = now.tv_sec;
			abstime.tv_nsec = now.tv_nsec + 500000000;
		} else {
			abstime.tv_sec = now.tv_sec + 1;
			abstime.tv_nsec = 500000000 - nsec;
		}

		rc = pthread_cond_timedwait(rcomm_cmd->cond_var,
		    rcomm_cmd->mutex, &abstime);
		err_num = errno;
		MTX_UNLOCK(rcomm_cmd->mutex);
	}

	return rc;
}

/*
 * This function handles error in replica's management interface
 * and inform replica_thread(data connection) regarding error
 */
static void
handle_mgmt_conn_error(replica_t *r, int sfd, struct epoll_event *events, int ev_count)
{
	int epollfd = r->epfd;
	int mgmtfd, mgmt_eventfd1;
	int i;
	mgmt_event_t *mevent;
	replica_t *r_ev;

	MTX_LOCK(&r->spec->rq_mtx);
	MTX_LOCK(&r->r_mtx);

	r->conn_closed++;
	if (r->conn_closed != 2) {
		//ASSERT(r->conn_closed == 1);
		/*
		 * case where error happened while sending HANDSHAKE or
		 * sending is successful but error from zvol_handshake or
		 * data connection is closed before this
		 */
		if ((r->iofd == -1) || (r->mgmt_eventfd2 == -1)) {
			TAILQ_FOREACH(r_ev, &(r->spec->rwaitq), r_waitnext) {
				if (r_ev == r) {
					TAILQ_REMOVE(&r->spec->rwaitq, r, r_waitnext);
					r->conn_closed++;
				}
			}
		}
		if (r->mgmt_eventfd2 != -1)
			inform_data_conn(r);
	} else {
		pthread_cond_signal(&r->r_cond);
	}

	mgmtfd = r->mgmt_fd;
	r->mgmt_fd = -1;
	MTX_UNLOCK(&r->r_mtx);
	MTX_UNLOCK(&r->spec->rq_mtx);

	close_fd(epollfd, mgmtfd);

	MTX_LOCK(&r->r_mtx);
	if (r->conn_closed != 2) {
		//ASSERT(r->conn_closed == 1);
		pthread_cond_wait(&r->r_cond, &r->r_mtx);
	}

	/* this need to be called after replica is removed from spec list
	 * data_conn thread should have removed from spec list as conn_closed is 2 */
	respond_with_error_for_all_outstanding_mgmt_ios(r);
	MTX_UNLOCK(&r->r_mtx);

	mgmt_eventfd1 = r->mgmt_eventfd1;
	r->mgmt_eventfd1 = -1;
	close_fd(epollfd, mgmt_eventfd1);

	for (i = 0; i < ev_count; i++) {
		if (events[i].data.fd == sfd) {
			continue;
		} else {
			if (events[i].data.ptr == NULL)
				continue;

			mevent = events[i].data.ptr;
			r_ev = mevent->r_ptr;

			if (r_ev != r)
				continue;

			if (mevent->fd == mgmt_eventfd1 ||
				mevent->fd == mgmtfd) {
				events[i].data.ptr = NULL;
			} else
				REPLICA_ERRLOG("unexpected fd(%d) for replica:%p\n", mevent->fd, r);
		}
	}

	if (r->dont_free != 1)
		free_replica(r);
}

/*
 * This function will handle event received from
 * replica_thread
 */
static int
handle_mgmt_event_fd(replica_t *replica)
{
	int rc = -1;

	do_drainfd(replica->mgmt_eventfd1);

	MTX_LOCK(&replica->r_mtx);
	if (replica->disconnect_conn == 1) {
		MTX_UNLOCK(&replica->r_mtx);
		return rc;
	}
	MTX_UNLOCK(&replica->r_mtx);

	rc = handle_write_data_event(replica);
	return rc;
}

/*
 * reads data on management fd
 */
int
handle_read_data_event(replica_t *replica)
{
	io_event_t revent;
	mgmt_cmd_t *mgmt_cmd;
	int rc = 0;

	MTX_LOCK(&replica->r_mtx);
	mgmt_cmd = TAILQ_FIRST(&replica->mgmt_cmd_queue);
	if (!(mgmt_cmd != NULL &&
		(mgmt_cmd->mgmt_cmd_state == READ_IO_RESP_HDR ||
		mgmt_cmd->mgmt_cmd_state == READ_IO_RESP_DATA))) {
		MTX_UNLOCK(&replica->r_mtx);
		/*
		 * Though we didn't send any IO query on management connection,
		 * We have a read event on management connection. Thats an error as
		 * management connection is not working in stateful manner. So we
		 * will print error message and does cleanup
		 */
		REPLICA_ERRLOG("unexpected read IO on mgmt connection..");
		return (-1);
	}

	MTX_UNLOCK(&replica->r_mtx);

	revent.fd = replica->mgmt_fd;
	revent.state = &(mgmt_cmd->mgmt_cmd_state);
	revent.io_hdr = replica->mgmt_io_resp_hdr;
	revent.io_data = (void **)(&(replica->mgmt_io_resp_data));
	revent.byte_count = &(mgmt_cmd->io_bytes);

	rc = read_io_resp(replica->spec, replica, &revent, mgmt_cmd);
	if (rc > 0) {
		if (rc > 1)
			REPLICA_NOTICELOG("read performed on management connection for more"
			    " than one IOs..");

		MTX_LOCK(&replica->r_mtx);
		clear_mgmt_cmd(replica, mgmt_cmd);
		MTX_UNLOCK(&replica->r_mtx);
		rc = handle_write_data_event(replica);
	}
	return (rc);
}

/*
 * initializes replication
 * - by starting listener to accept mgmt connections
 * - reads data on accepted mgmt connection
 */
void *
init_replication(void *arg __attribute__((__unused__)))
{
	struct epoll_event event, *events;
	int rc, sfd, event_count, i;
	int64_t epfd;
	replica_t *r;
	int timeout;
	struct timespec last, now, diff;
	mgmt_event_t *mevent;

	//Create a listener for management connections from replica
	const char* externalIP = getenv("externalIP");
	if((sfd = cstor_ops.conn_listen(externalIP, 6060, 32, 1)) < 0) {
		REPLICA_LOG("conn_listen() failed, errorno:%d sfd:%d", errno, sfd);
		exit(EXIT_FAILURE);
	}

	epfd = epoll_create1(0);
	event.data.fd = sfd;
	event.events = EPOLLIN | EPOLLET | EPOLLERR | EPOLLHUP;
	rc = epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &event);
	if (rc == -1) {
		REPLICA_ERRLOG("epoll_ctl() failed, errrno:%d", errno);
		exit(EXIT_FAILURE);
	}

	events = calloc(MAXEVENTS, sizeof(event));
	timeout = 60 * 1000;	// 60 seconds
	clock_gettime(CLOCK_MONOTONIC, &last);

	while (1) {
		//Wait for management connections(on sfd) and management commands(on mgmt_rfds[]) from replicas
		event_count = epoll_wait(epfd, events, MAXEVENTS, timeout);
		if (event_count < 0) {
			if (errno == EINTR)
				continue;
			REPLICA_ERRLOG("epoll_wait ret %d err %d.. better to restart listener\n", event_count, errno);
			continue;
		}

		for(i=0; i< event_count; i++) {
			if (events[i].events & EPOLLHUP || events[i].events & EPOLLERR ||
				events[i].events & EPOLLRDHUP) {
				if (events[i].data.fd == sfd) {
					REPLICA_ERRLOG("epoll event %d on fd %d... better to restart listener\n",
					    events[i].events, events[i].data.fd);
					exit(EXIT_FAILURE);	//Here, we can exit o/w need to perform cleanup for all replica
				} else {
					if (events[i].data.ptr == NULL)
						continue;
					mevent = events[i].data.ptr;
					r = mevent->r_ptr;
					REPLICA_ERRLOG("epoll event %d on replica %p\n", events[i].events, r);
					handle_mgmt_conn_error(r, sfd, events, event_count);
				}
			} else {
				if (events[i].data.fd == sfd) {
					//Accept management connections from replicas and add the replicas to replica queue
					accept_mgmt_conns(epfd, sfd);
				} else {
					if (events[i].data.ptr == NULL)
						continue;

					mevent = events[i].data.ptr;
					r = mevent->r_ptr;

					rc = 0;
					if (events[i].events & EPOLLIN) {
						if (mevent->fd == r->mgmt_fd)
							rc = handle_read_data_event(r);
						else
							rc = handle_mgmt_event_fd(r);
					}
					if (rc == -1)
						handle_mgmt_conn_error(r, sfd, events, event_count);

					rc = 0;
					if (events[i].events & EPOLLOUT)
						//ASSERT(mevent->fd == r->mgmt_fd);
						rc = handle_write_data_event(r);
					if (rc == -1)
						handle_mgmt_conn_error(r, sfd, events, event_count);
				}
			}
		}

		// send replica_status query to degraded replicas at interval of 60 seconds
		timesdiff(last, now, diff);
		if (diff.tv_sec >= 60) {
			spec_t *spec = NULL;
			MTX_LOCK(&specq_mtx);
			TAILQ_FOREACH(spec, &spec_q, spec_next) {
				ask_replica_status_all(spec);
			}
			MTX_UNLOCK(&specq_mtx);
			clock_gettime(CLOCK_MONOTONIC, &last);
		}
	}

	free (events);
	close (sfd);
	close (epfd);
	return EXIT_SUCCESS;
}

/*
 * initialize spec queue and mutex
 */
int
initialize_replication()
{
	//Global initializers for replication library
	int rc;
	TAILQ_INIT(&spec_q);
	rc = pthread_mutex_init(&specq_mtx, NULL);
	if (rc != 0) {
		REPLICA_ERRLOG("Failed to init specq_mtx err(%d)\n", errno);
		return -1;
	}
	return 0;
}

int
initialize_volume(spec_t *spec, int replication_factor, int consistency_factor)
{
	int rc;
	pthread_t deadlist_cleanup_thread;

	spec->io_seq = 0;
	TAILQ_INIT(&spec->rcommon_waitq);
	TAILQ_INIT(&spec->rq);
	TAILQ_INIT(&spec->rwaitq);

        if(init_mempool(&spec->rcommon_deadlist, rcmd_mempool_count, 0, 0,
            "rcmd_mempool", NULL, NULL, NULL, false)) {
		return -1;
	}

	spec->replica_count = 0;
	spec->replication_factor = replication_factor;
	spec->consistency_factor = consistency_factor;
	spec->healthy_rcount = 0;
	spec->degraded_rcount = 0;
	spec->ready = false;

	rc = pthread_mutex_init(&spec->rcommonq_mtx, NULL);
	if (rc != 0) {
		REPLICA_ERRLOG("Failed to ini rcommonq mtx err(%d)\n", errno);
		return -1;
	}

	rc = pthread_mutex_init(&spec->rq_mtx, NULL);
	if (rc != 0) {
		REPLICA_ERRLOG("Failed to init rq_mtx err(%d)\n", errno);
		return -1;
	}

	rc = pthread_create(&deadlist_cleanup_thread, NULL, &cleanup_deadlist,
			(void *)spec);
	if (rc != 0) {
		ISTGT_ERRLOG("pthread_create(replicator_thread) failed\n");
		return -1;
	}

	MTX_LOCK(&specq_mtx);
	TAILQ_INSERT_TAIL(&spec_q, spec, spec_next);
	MTX_UNLOCK(&specq_mtx);

	return 0;
}

/*
 * This function initializes mempool for replica's command(rcmd_t) and
 * spec's command(rcommon_cmd_t)
 */
int
initialize_replication_mempool(bool should_fail)
{
	int rc = 0;

	rc = init_mempool(&rcmd_mempool, rcmd_mempool_count, sizeof (rcmd_t), 0,
	    "rcmd_mempool", NULL, NULL, NULL, true);
	if (rc == -1) {
		ISTGT_ERRLOG("Failed to create mempool for command\n");
		goto error;
	} else if (rc) {
		ISTGT_NOTICELOG("rcmd mempool initialized with %u entries\n",
		    rcmd_mempool.length);
		if (should_fail) {
			goto error;
		}
		rc = 0;
	}

	rc = init_mempool(&rcommon_cmd_mempool, rcommon_cmd_mempool_count,
	    sizeof (rcommon_cmd_t), 0, "rcommon_mempool", NULL, NULL, NULL, true);
	if (rc == -1) {
		ISTGT_ERRLOG("Failed to create mempool for command\n");
		goto error;
	} else if (rc) {
		ISTGT_NOTICELOG("rcmd mempool initialized with %u entries\n",
		    rcommon_cmd_mempool.length);
		if (should_fail) {
			goto error;
		}
		rc = 0;
	}

	goto exit;

error:
	if (rcmd_mempool.ring)
		destroy_mempool(&rcmd_mempool);
	if (rcommon_cmd_mempool.ring)
		destroy_mempool(&rcommon_cmd_mempool);

exit:
	return rc;
}

/*
 * This function destroys mempool created for replica's command(rcmd_t)
 * and spec's command(rcommon_cmd_t)
 */
int
destroy_relication_mempool(void)
{
	int rc = 0;

	rc = destroy_mempool(&rcmd_mempool);
	if (rc) {
		ISTGT_ERRLOG("Failed to destroy mempool for rcmd.. err(%d)\n",
		    rc);
		goto exit;
	}

	rc = destroy_mempool(&rcommon_cmd_mempool);
	if (rc) {
		ISTGT_ERRLOG("Failed to destroy mempool for rcommon_cmd.."
		    " err(%d)\n", rc);
		goto exit;
	}

exit:
	return rc;
}

/*
 * destroy response recieved from replica for a rcommon_cmd
 */
static void
destroy_resp_list(rcommon_cmd_t *rcomm_cmd)
{
	int i;

	for (i = 0; i < rcomm_cmd->copies_sent; i++) {
		if (rcomm_cmd->resp_list[i].data_ptr) {
			free(rcomm_cmd->resp_list[i].data_ptr);
		}
	}
}

/*
 * perform cleanup for completed rcommon_cmd (whose response is
 * sent back to the client)
 */
void *
cleanup_deadlist(void *arg)
{
	spec_t *spec = (spec_t *)arg;
	rcommon_cmd_t *rcomm_cmd;
	int i, count = 0, entry_count = 0;

	while (1) {
		entry_count = get_num_entries_from_mempool(&spec->rcommon_deadlist);
		while (entry_count) {
			count = 0;
			rcomm_cmd = get_from_mempool(&spec->rcommon_deadlist);

			for (i = 0; i < rcomm_cmd->copies_sent; i++) {
				if (rcomm_cmd->resp_list[i].status)
					count++;
			}

			if (count == rcomm_cmd->copies_sent) {
				destroy_resp_list(rcomm_cmd);

				for (i=1; i<rcomm_cmd->iovcnt + 1; i++)
					xfree(rcomm_cmd->iov[i].iov_base);

				memset(rcomm_cmd, 0, sizeof(rcommon_cmd_t));
				put_to_mempool(&rcommon_cmd_mempool, rcomm_cmd);
			} else {
				put_to_mempool(&spec->rcommon_deadlist, rcomm_cmd);
			}
			entry_count--;
		}
		sleep(1);	//add predefined time here
	}
	return (NULL);
}

/*
int
remove_volume(spec_t *spec) {

	rcommon_cmd_t *cmd, *next_cmd = NULL;
	rcmd_t *rcmd, *next_rcmd = NULL;

	//Remove all cmds from rwaitq and rblockedq of all replicas
	TAILQ_FOREACH(replica, &spec->rq, r_next) {
		if(replica == NULL) {
			perror("Replica not present");
			exit(EXIT_FAILURE);
		}
		MTX_LOCK(&replica->q_mtx);
		rcmd = TAILQ_FIRST(&replica->rwaitq);
		while (rcmd) {
			rcmd->status = -1;
			next_rcmd = TAILQ_NEXT(rcmd, rwait_cmd_next);
			TAILQ_REMOVE(&rwaitq, rcmd, rwait_cmd_next);
			rcmd = next_rcmd;
		}

		rcmd = TAILQ_FIRST(&replica->rblockedq);
		while (rcmd) {
			rcmd->status = -1;
			next_rcmd = TAILQ_NEXT(rcmd, rblocked_cmd_next);
			TAILQ_REMOVE(&rwaitq, rcmd, rblocked_cmd_next);
			rcmd = next_rcmd;
		}
		MTX_UNLOCK(replica->q_mtx);
	}

	//Remove all cmds from rcommon_sendq, rcommon_waitq, and rcommon_pendingq
	MTX_LOCK(&spec->rcommonq_mtx);
	cmd = TAILQ_FIRST(&spec->rcommon_sendq);
	while (cmd) {
		cmd->status = -1;
		cmd->completed = 1; \
		next_cmd = TAILQ_NEXT(cmd, send_cmd_next);
		TAILQ_REMOVE(&rcommon_sendq, cmd, send_cmd_next);
		cmd = next_cmd;
	}
	cmd = TAILQ_FIRST(&spec->rcommon_waitq);
	while (cmd) {
		cmd->status = -1;
		cmd->completed = 1; \
		next_cmd = TAILQ_NEXT(cmd, wait_cmd_next);
		TAILQ_REMOVE(&rcommon_waitq, cmd, wait_cmd_next);
		cmd = next_cmd;
	}
	cmd = TAILQ_FIRST(&spec->rcommon_pendingq);
	while (cmd) {
		cmd->status = -1;
		cmd->completed = 1; \
		next_cmd = TAILQ_NEXT(cmd, pending_cmd_next);
		TAILQ_REMOVE(&rcommon_pendingq, cmd, pending_cmd_next);
		cmd = next_cmd;
	}
	MTX_UNLOCK(spec->rcommonq_mtx);

	for(i=0; i<spec->luworkers; i++) {
		pthread_cond_signal(&spec->luworker_cond[i]);
	}

	pthread_mutex_destroy(&spec->rq_mtx);
	pthread_cond_destroy(&spec->rq_cond);
	MTX_LOCK(&specq_mtx);
	TAILQ_REMOVE(&spec_q, spec, spec_next);
	MTX_UNLOCK(&specq_mtx);
}
*/