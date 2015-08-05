/*
 * Copyright (c) 2012-2014 CEA
 * Dominique Martinet <dominique.martinet@cea.fr>
 * contributeur : William Allen Simpson <bill@cohortfs.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Sun Microsystems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>
#include <sys/cdefs.h>

#include "namespace.h"
#include <sys/types.h>

#include <netinet/in.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <rpc/types.h>
#include <rpc/xdr.h>
#include <rpc/xdr_ioq.h>
#include <rpc/rpc.h>
#include "un-namespace.h"

//#include <infiniband/verbs.h>
#include "rpc_rdma.h"

/* NOTA BENE: as in xdr_ioq.c, although indications of failure are returned,
 * they are rarely checked.
 */

#define CALLQ_SIZE (2)

static struct	xdr_ops xdr_rdma_ops_recycle;

static const struct	xdr_ops xdr_rdma_ops_aligned;
static const struct	xdr_ops xdr_rdma_ops_unaligned;

#define x_xprt(xdrs) ((RDMAXPRT *)((xdrs)->x_lib[1]))

//#define rpcrdma_dump_msg(data, comment, xid)

#ifndef rpcrdma_dump_msg
#define DUMP_BYTES_PER_GROUP (4)
#define DUMP_GROUPS_PER_LINE (4)
#define DUMP_BYTES_PER_LINE (DUMP_BYTES_PER_GROUP * DUMP_GROUPS_PER_LINE)

static void
rpcrdma_dump_msg(struct xdr_ioq_uv *data, char *comment, uint32_t xid)
{
	char *buffer;
	uint8_t *datum = data->v.vio_head;
	int sized = ioquv_length(data);
	int buffered = (((sized / DUMP_BYTES_PER_LINE) + 1 /*partial line*/)
			* (12 /* heading */
			   + (((DUMP_BYTES_PER_GROUP * 2 /*%02X*/) + 1 /*' '*/)
			      * DUMP_GROUPS_PER_LINE)))
			+ 1 /*'\0'*/;
	int i = 0;
	int m = 0;

	xid = ntohl(xid);
	if (sized == 0) {
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"rpcrdma 0x%" PRIx32 "(%" PRIu32 ") %s?",
			xid, xid, comment);
		return;
	}
	buffer = (char *)mem_alloc(buffered);

	while (sized > i) {
		int j = sized - i;
		int k = j < DUMP_BYTES_PER_LINE ? j : DUMP_BYTES_PER_LINE;
		int l = 0;
		int r = sprintf(&buffer[m], "\n%10d:", i);	/* heading */

		if (r < 0)
			goto quit;
		m += r;

		for (; l < k; l++) {
			if (l % DUMP_BYTES_PER_GROUP == 0)
				buffer[m++] = ' ';

			r = sprintf(&buffer[m], "%02X", datum[i++]);
			if (r < 0)
				goto quit;
			m += r;
		}
	}
quit:
	buffer[m] = '\0';	/* in case of error */
	__warnx(TIRPC_DEBUG_FLAG_XDR,
		"rpcrdma 0x%" PRIx32 "(%" PRIu32 ") %s:%s\n",
		xid, xid, comment, buffer);
	mem_free(buffer, buffered);
}
#endif /* rpcrdma_dump_msg */

/***********************************/
/****** Utilities for buffers ******/
/***********************************/


/***********************/
/****** Callbacks ******/
/***********************/

/* note parameter order matching svc.h svc_req callbacks */

static void
xdr_rdma_respond_callback(struct rpc_rdma_cbc *cbc, RDMAXPRT *xprt)
{
	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"%s() %p[%u] cbc %p\n",
		__func__, xprt, xprt->state, cbc);

	mutex_lock(&xprt->waitq.ioq_uv.uvqh.qmutex);
	TAILQ_REMOVE(&xprt->waitq.ioq_uv.uvqh.qh, &cbc->workq.ioq_s, q);
	(xprt->waitq.ioq_uv.uvqh.qcount)--;
	mutex_unlock(&xprt->waitq.ioq_uv.uvqh.qmutex);

	xdr_ioq_destroy(&cbc->workq, sizeof(*cbc));
	SVC_RELEASE(&xprt->xprt, SVC_RELEASE_FLAG_NONE);
}

static void
xdr_rdma_destroy_callback(struct rpc_rdma_cbc *cbc, RDMAXPRT *xprt)
{
	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"%s() %p[%u] cbc %p\n",
		__func__, xprt, xprt->state, cbc);

	mutex_lock(&xprt->waitq.ioq_uv.uvqh.qmutex);
	TAILQ_REMOVE(&xprt->waitq.ioq_uv.uvqh.qh, &cbc->workq.ioq_s, q);
	(xprt->waitq.ioq_uv.uvqh.qcount)--;
	mutex_unlock(&xprt->waitq.ioq_uv.uvqh.qmutex);

	xdr_ioq_destroy(&cbc->workq, sizeof(*cbc));
	SVC_RELEASE(&xprt->xprt, SVC_RELEASE_FLAG_NONE);
}

/**
 * xdr_rdma_wait_callback: send/recv callback that just unlocks a mutex.
 *
 */
static void
xdr_rdma_wait_callback(struct rpc_rdma_cbc *cbc, RDMAXPRT *xprt)
{
	mutex_t *lock = cbc->callback_arg;

	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"%s() %p[%u] cbc %p\n",
		__func__, xprt, xprt->state, cbc);

	mutex_unlock(lock);
	SVC_RELEASE(&xprt->xprt, SVC_RELEASE_FLAG_NONE);
}

/**
 * xdr_rdma_warn_callback: send/recv callback that just unlocks a mutex.
 *
 */
static void
xdr_rdma_warn_callback(struct rpc_rdma_cbc *cbc, RDMAXPRT *xprt)
{
	mutex_t *lock = cbc->callback_arg;

	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"%s() %p[%u] cbc %p\n",
		__func__, xprt, xprt->state, cbc);

	mutex_unlock(lock);
	SVC_RELEASE(&xprt->xprt, SVC_RELEASE_FLAG_NONE);
}

/***********************************/
/***** Utilities from Mooshika *****/
/***********************************/

/**
 * xdr_rdma_post_recv_n: Post a receive buffer.
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param[IN] xprt
 * @param[INOUT] cbc	CallBack Context xdr_ioq and xdr_ioq_uv(s)
 *
 * Must be set in advance:
 * @param[IN] positive_cb	function that'll be called when done
 * @param[IN] negative_cb	function that'll be called on error
 * @param[IN] callback_arg	argument to give to the callback

 * @return 0 on success, the value of errno on error
 */
static int
xdr_rdma_post_recv_n(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc)
{
	struct poolq_entry *have;
	int ret;
	int i = 0;

	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() xprt state missing",
			__func__);
		return EINVAL;
	}

	switch (xprt->state) {
	case RDMAXS_CONNECTED:
	case RDMAXS_ROUTE_RESOLVED:
	case RDMAXS_CONNECT_REQUEST:
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() %p[%u] cbc %p posting recv",
			__func__, xprt, xprt->state, cbc);
		break;
	default:
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() %p[%u] != "
			"connect request, connected, or resolved",
			__func__, xprt, xprt->state);
		return EINVAL;
	}

	TAILQ_FOREACH(have, &cbc->workq.ioq_uv.uvqh.qh, q) {
		struct ibv_mr *mr = IOQ_(have)->u.uio_p2;

		if (!mr) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() Missing mr: Not requesting.",
				__func__);
			return EINVAL;
		}

		cbc->sg_list[i].addr = (uintptr_t)(IOQ_(have)->v.vio_head);
		cbc->sg_list[i].length = ioquv_length(IOQ_(have));
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() %" PRIx64 ", %" PRIu32 " [%" PRIx32 "]",
			__func__,
			cbc->sg_list[i].addr,
			cbc->sg_list[i].length,
			mr->lkey);
		cbc->sg_list[i++].lkey = mr->lkey;
	}

	cbc->wr.rwr.next = NULL;
	cbc->wr.rwr.wr_id = (uintptr_t)cbc;
	cbc->wr.rwr.sg_list = cbc->sg_list;
	cbc->wr.rwr.num_sge = i;

	if (xprt->srq)
		ret = ibv_post_srq_recv(xprt->srq, &cbc->wr.rwr,
					&xprt->bad_recv_wr);
	else
		ret = ibv_post_recv(xprt->qp, &cbc->wr.rwr,
					&xprt->bad_recv_wr);

	if (ret) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] cbc %p ibv_post_recv failed: %s (%d)",
			__func__, xprt, xprt->state, cbc, strerror(ret), ret);
		return ret; // FIXME np_uerror(ret)
	}

	return 0;
}

/**
 * xdr_rdma_post_recv_cb: Post a receive buffer with standard callbacks.
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param[IN] xprt
 * @param[INOUT] cbc	CallBack Context xdr_ioq and xdr_ioq_uv(s)
 *
 * @return 0 on success, the value of errno on error
 */
static int
xdr_rdma_post_recv_cb(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc)
{
	cbc->positive_cb = (rpc_rdma_callback_t)xprt->xa->request_cb;
	cbc->negative_cb = xdr_rdma_destroy_callback;
	cbc->callback_arg = NULL;
	return xdr_rdma_post_recv_n(xprt, cbc);
}

static void
xdr_rdma_post_recycle(XDR *xdrs)
{
	xdr_rdma_post_recv_cb((RDMAXPRT *)(xdrs->x_lib[1]),
				(struct rpc_rdma_cbc *)xdrs);
}

/**
 * Post a send buffer.
 *
 * @param[IN] xprt
 * @param[IN] cbc	CallBack Context xdr_ioq and xdr_ioq_uv(s)
 * @param[IN] opcode
 *
 * Must be set in advance:
 * @param[IN] positive_cb	function that'll be called when done
 * @param[IN] negative_cb	function that'll be called on error
 * @param[IN] callback_arg	argument to give to the callback
 *
 * @return 0 on success, the value of errno on error
 */
static int
xdr_rdma_post_send_n(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc,
		     struct rpcrdma_segment *rs, enum ibv_wr_opcode opcode)
{
	struct poolq_entry *have;
	int ret;
	int i = 0;
	uint32_t totalsize = 0;

	if (!xprt) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() xprt state missing",
			__func__);
		return EINVAL;
	}

	switch (xprt->state) {
	case RDMAXS_CONNECTED:
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() %p[%u] cbc %p posting a send with op %d",
			__func__, xprt, xprt->state, cbc, opcode);
		break;
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] != "
			"connected",
			__func__, xprt, xprt->state);
		return EINVAL;
	}

	// opcode-specific checks:
	switch (opcode) {
	case IBV_WR_RDMA_WRITE:
	case IBV_WR_RDMA_READ:
		if (!rs) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() Cannot do rdma without a remote location!",
				__func__);
			return EINVAL;
		}
		break;
	case IBV_WR_SEND:
	case IBV_WR_SEND_WITH_IMM:
		break;
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() unsupported op code: %d",
			__func__, opcode);
		return EINVAL;
	}

	TAILQ_FOREACH(have, &cbc->workq.ioq_uv.uvqh.qh, q) {
		struct ibv_mr *mr = IOQ_(have)->u.uio_p2;
		uint32_t length = ioquv_length(IOQ_(have));

		if (!length) {
			__warnx(TIRPC_DEBUG_FLAG_XDR,
				"%s() Empty buffer: Not sending.",
				__func__);
			return EINVAL;
		}
		if (!mr) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() Missing mr: Not sending.",
				__func__);
			return EINVAL;
		}

		cbc->sg_list[i].addr = (uintptr_t)(IOQ_(have)->v.vio_head);
		cbc->sg_list[i].length = length;
		__warnx(TIRPC_DEBUG_FLAG_XDR,
			"%s() %" PRIx64 ", %" PRIu32 " [%" PRIx32 "]",
			__func__,
			cbc->sg_list[i].addr,
			cbc->sg_list[i].length,
			mr->lkey);
		cbc->sg_list[i++].lkey = mr->lkey;
		totalsize += length;
	}

	cbc->wr.wwr.next = NULL;
	cbc->wr.wwr.wr_id = (uint64_t)cbc;
	cbc->wr.wwr.opcode = opcode;
//FIXME	cbc->wr.wwr.imm_data = htonl(data->imm_data);
	cbc->wr.wwr.send_flags = IBV_SEND_SIGNALED;
	cbc->wr.wwr.sg_list = cbc->sg_list;
	cbc->wr.wwr.num_sge = i;

	if (rs) {
		cbc->wr.wwr.wr.rdma.rkey = ntohl(rs->rs_handle);
		cbc->wr.wwr.wr.rdma.remote_addr =
			xdr_decode_hyper(&rs->rs_offset);

		if (totalsize > ntohl(rs->rs_length)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() trying to send or read a buffer "
				"bigger than the remote buffer "
				"(shall we truncate?)",
				__func__);
			return EMSGSIZE;
		}
	}

	ret = ibv_post_send(xprt->qp, &cbc->wr.wwr, &xprt->bad_send_wr);
	if (ret) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] cbc %p ibv_post_send failed: %s (%d)",
			__func__, xprt, xprt->state, cbc, strerror(ret), ret);
		return ret; // FIXME np_uerror(ret)
	}

	return 0;
}

/**
 * Post a send buffer with standard callbacks.
 *
 * @param[IN] xprt
 * @param[IN] cbc	CallBack Context xdr_ioq and xdr_ioq_uv(s)
 *
 * @return 0 on success, the value of errno on error
 */
static inline int
xdr_rdma_post_send_cb(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc)
{
	cbc->positive_cb = xdr_rdma_respond_callback;
	cbc->negative_cb = xdr_rdma_destroy_callback;
	cbc->callback_arg = cbc;
	return xdr_rdma_post_send_n(xprt, cbc, NULL, IBV_WR_SEND);
}

#ifdef UNUSED
/**
 * Post a receive buffer and waits for _that one and not any other_ to be filled.
 * Generally a bad idea to use that one unless only that one is used.
 *
 * @param[IN] xprt
 * @param[INOUT] cbc	CallBack Context xdr_ioq and xdr_ioq_uv(s)
 *
 * @return 0 on success, the value of errno on error
 */
static int
xdr_rdma_wait_recv_n(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc)
{
	mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	cbc->positive_cb = xdr_rdma_wait_callback;
	cbc->negative_cb = xdr_rdma_warn_callback;
	cbc->callback_arg = &lock;

	mutex_lock(&lock);
	ret = xdr_rdma_post_recv_n(xprt, cbc);

	if (!ret) {
		mutex_lock(&lock);
		mutex_unlock(&lock);
	}
	mutex_destroy(&lock);

	return ret;
}

/**
 * Post a send buffer and waits for that one to be completely sent
 * @param[IN] xprt
 * @param[IN] cbc	CallBack Context xdr_ioq and xdr_ioq_uv(s)
 *
 * @return 0 on success, the value of errno on error
 */
static int
xdr_rdma_wait_send_n(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc)
{
	mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	int ret;

	cbc->positive_cb = xdr_rdma_wait_callback;
	cbc->negative_cb = xdr_rdma_warn_callback;
	cbc->callback_arg = &lock;

	mutex_lock(&lock);
	ret = xdr_rdma_post_send_n(xprt, cbc, NULL, IBV_WR_SEND);

	if (!ret) {
		mutex_lock(&lock);
		mutex_unlock(&lock);
	}
	mutex_destroy(&lock);

	return ret;
}

static inline int
xdr_rdma_post_read_cb(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc,
		      struct rpcrdma_segment *rs)
{
	cbc->positive_cb = xdr_rdma_respond_callback;
	cbc->negative_cb = xdr_rdma_destroy_callback;
	cbc->callback_arg = cbc;
	return xdr_rdma_post_send_n(xprt, cbc, rs, IBV_WR_RDMA_READ);
}
#endif /* UNUSED */

/**
 * Post a write buffer with standard callbacks.
 *
 * @param[IN] xprt
 * @param[IN] cbc	CallBack Context xdr_ioq and xdr_ioq_uv(s)
 *
 * @return 0 on success, the value of errno on error
 */
static inline int
xdr_rdma_post_write_cb(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc,
		       struct rpcrdma_segment *rs)
{
	cbc->positive_cb = xdr_rdma_respond_callback;
	cbc->negative_cb = xdr_rdma_destroy_callback;
	cbc->callback_arg = cbc;
	return xdr_rdma_post_send_n(xprt, cbc, rs, IBV_WR_RDMA_WRITE);
}

static int
xdr_rdma_wait_read_n(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc,
		     struct rpcrdma_segment *rs)
{
	mutex_t lock = MUTEX_INITIALIZER;
	int ret;

	cbc->positive_cb = xdr_rdma_wait_callback;
	cbc->negative_cb = xdr_rdma_warn_callback;
	cbc->callback_arg = &lock;

	mutex_lock(&lock);
	ret = xdr_rdma_post_send_n(xprt, cbc, rs, IBV_WR_RDMA_READ);

	if (!ret) {
		mutex_lock(&lock);
		mutex_unlock(&lock);
	}
	mutex_destroy(&lock);

	return ret;
}

#ifdef UNUSED
static int
xdr_rdma_wait_write_n(RDMAXPRT *xprt, struct rpc_rdma_cbc *cbc,
		      struct rpcrdma_segment *rs)
{
	mutex_t lock = MUTEX_INITIALIZER;
	int ret;

	cbc->positive_cb = xdr_rdma_wait_callback;
	cbc->negative_cb = xdr_rdma_warn_callback;
	cbc->callback_arg = &lock;

	mutex_lock(&lock);
	ret = xdr_rdma_post_send_n(xprt, cbc, rs, IBV_WR_RDMA_WRITE);

	if (!ret) {
		mutex_lock(&lock);
		mutex_unlock(&lock);
	}
	mutex_destroy(&lock);

	return ret;
}
#endif /* UNUSED */

/***********************************/
/****** Utilities for rpcrdma ******/
/***********************************/

#define _md(ptr) ((struct rpcrdma_msg *)ptr)
#define _rc(ptr) ((struct rpcrdma_read_chunk*)ptr)

typedef struct rpcrdma_write_array _wa_t;
#define _wa(ptr) ((struct rpcrdma_write_array*)ptr)

static inline void
xdr_rdma_skip_read_list(uint32_t **pptr)
{
	while (_rc(*pptr)->rc_discrim) {
		*pptr += sizeof(struct rpcrdma_read_chunk)
			 / sizeof(**pptr);
	}
	(*pptr)++;
}

static inline void
xdr_rdma_skip_write_array(uint32_t **pptr)
{
	if (_wa(*pptr)->wc_discrim) {
		*pptr += (sizeof(struct rpcrdma_write_array)
			  + sizeof(struct rpcrdma_write_chunk)
			    * ntohl(_wa(*pptr)->wc_nchunks))
			 / sizeof(**pptr);
	}
	(*pptr)++;
}

static inline void
xdr_rdma_skip_reply_array(uint32_t **pptr)
{
	if (_wa(*pptr)->wc_discrim) {
		*pptr += (sizeof(struct rpcrdma_write_array)
			  + sizeof(struct rpcrdma_write_chunk)
			    * ntohl(_wa(*pptr)->wc_nchunks))
			 / sizeof(**pptr);
	} else {
		(*pptr)++;
	}
}

static inline struct rpcrdma_read_chunk *
xdr_rdma_get_read_list(char *rmsg)
{
	return _rc(_md(rmsg)->rm_body.rm_chunks);
}

#ifdef UNUSED
static inline _wa_t *
xdr_rdma_get_write_array(struct rpcrdma_msg *rmsg)
{
	uint32_t *ptr = rmsg->rm_body.rm_chunks;

	xdr_rdma_skip_read_list(&ptr);

	return _wa(ptr);
}
#endif /* UNUSED */

static inline _wa_t *
xdr_rdma_get_reply_array(char *data)
{
	uint32_t *ptr = _md(data)->rm_body.rm_chunks;

	xdr_rdma_skip_read_list(&ptr);
	xdr_rdma_skip_write_array(&ptr);

	return _wa(ptr);
}

static inline uint32_t *
xdr_rdma_skip_header(struct rpcrdma_msg *rmsg)
{
	uint32_t *ptr = rmsg->rm_body.rm_chunks;

	xdr_rdma_skip_read_list(&ptr);
	xdr_rdma_skip_write_array(&ptr);
	xdr_rdma_skip_reply_array(&ptr);

	return ptr;
}

static inline uintptr_t
xdr_rdma_header_length(struct rpcrdma_msg *rmsg)
{
	uint32_t *ptr = xdr_rdma_skip_header(rmsg);

	return ((uintptr_t)ptr - (uintptr_t)rmsg);
}

#ifdef UNUSED
int
xdr_rdma_encode_error(struct svcxprt_rdma *xprt,
			      struct rpcrdma_msg *rmsgp,
			      enum xdr_rdma_errcode err, u32 *va)
{
	u32 *startp = va;

	*va++ = htonl(rmsgp->rm_xid);
	*va++ = htonl(rmsgp->rm_vers);
	*va++ = htonl(xprt->sc_max_requests);
	*va++ = htonl(RDMA_ERROR);
	*va++ = htonl(err);
	if (err == ERR_VERS) {
		*va++ = htonl(RPCRDMA_VERSION);
		*va++ = htonl(RPCRDMA_VERSION);
	}

	return (int)((unsigned long)va - (unsigned long)startp);
}

void
xdr_rdma_encode_reply_array(_wa_t *ary, int chunks)
{
	ary->wc_discrim = xdr_one;
	ary->wc_nchunks = htonl(chunks);
}

void
xdr_rdma_encode_array_chunk(_wa_t *ary, int chunk_no, u32 rs_handle,
			    u64 rs_offset, u32 write_len)
{
	struct rpcrdma_segment *seg = &ary->wc_array[chunk_no].wc_target;
	seg->rs_handle = htonl(rs_handle);
	seg->rs_length = htonl(write_len);
	xdr_encode_hyper((u32 *) &seg->rs_offset, rs_offset);
}

void
xdr_rdma_encode_reply_header(struct svcxprt_rdma *xprt,
				  struct rpcrdma_msg *rdma_argp,
				  struct rpcrdma_msg *rdma_resp,
				  enum rpcrdma_proc rdma_type)
{
	rdma_resp->rm_xid = htonl(rdma_argp->rm_xid);
	rdma_resp->rm_vers = htonl(rdma_argp->rm_vers);
	rdma_resp->rm_credit = htonl(xprt->sc_max_requests);
	rdma_resp->rm_type = htonl(rdma_type);

	/* Encode <nul> chunks lists */
	rdma_resp->rm_body.rm_chunks[0] = xdr_zero;
	rdma_resp->rm_body.rm_chunks[1] = xdr_zero;
	rdma_resp->rm_body.rm_chunks[2] = xdr_zero;
}
#endif /* UNUSED */

/****************************/
/****** Main functions ******/
/****************************/

/* post recv buffers.
 * keep at least 2 spare waiting for calls,
 * the remainder can be used for incoming rdma buffers.
 */
static void
xdr_rdma_callq(RDMAXPRT *xprt)
{
	struct poolq_entry *have =
		xdr_ioq_uv_fetch(&xprt->waitq, &xprt->cbqh,
				 "callq context", 1, IOQ_FLAG_NONE);
	struct rpc_rdma_cbc *cbc = (struct rpc_rdma_cbc *)(_IOQ(have));

	have = xdr_ioq_uv_fetch(&cbc->workq, &xprt->inbufs.uvqh,
				"callq buffer", 1, IOQ_FLAG_NONE);

	/* input positions */
	IOQ_(have)->v.vio_head = IOQ_(have)->v.vio_base;
	IOQ_(have)->v.vio_tail = IOQ_(have)->v.vio_wrap;
/*	IOQ_(have)->v.vio_wrap = IOQ_(have)->v.vio_base + xprt->recvsize; */

	cbc->workq.ioq_uv.uvq_fetch =
	cbc->holdq.ioq_uv.uvq_fetch = xdr_ioq_uv_fetch_nothing;
	cbc->workq.xdrs[0].x_ops =
	cbc->holdq.xdrs[0].x_ops = &xdr_rdma_ops_recycle;

	cbc->workq.xdrs[0].x_op = XDR_ENCODE;
	cbc->holdq.xdrs[0].x_op = XDR_DECODE;
	cbc->workq.xdrs[0].x_lib[1] =
	cbc->holdq.xdrs[0].x_lib[1] = xprt;

	xdr_rdma_post_recv_cb(xprt, cbc);
}

void
xdr_rdma_destroy(XDR *xdrs)
{
	RDMAXPRT *xprt;

	if (!xdrs) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() no xdrs?",
			__func__);
		return;
	}
	xprt = x_xprt(xdrs);

	if (xprt->mr) {
		ibv_dereg_mr(xprt->mr);
		xprt->mr = NULL;
	}

	xdr_ioq_destroy_pool(&xprt->waitq.ioq_uv.uvqh);

	/* must be after queues, xdr_ioq_destroy() moves them here */
	xdr_ioq_release(&xprt->inbufs.uvqh);
	poolq_head_destroy(&xprt->inbufs.uvqh);
	xdr_ioq_release(&xprt->outbufs.uvqh);
	poolq_head_destroy(&xprt->outbufs.uvqh);

	/* must be after pools */
	if (xprt->buffer_aligned) {
		mem_free(xprt->buffer_aligned, xprt->buffer_total);
		xprt->buffer_aligned = NULL;
	}

	xdrs->x_lib[0] = NULL;
	xdrs->x_lib[1] = NULL;
}

/*
 * initializes a stream descriptor for a memory buffer.
 *
 * XDR has already been created and passed as arg.
 *
 * credits is the number of buffers used
 */
int
xdr_rdma_create(XDR *xdrs, RDMAXPRT *xprt, const u_int sendsize,
		const u_int recvsize, const u_int flags)
{
	uint8_t *b;
	long ps = sysconf(_SC_PAGESIZE);

	if (!xprt->pd || !xprt->pd->pd) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() %p[%u] xdr %p missing Protection Domain",
			__func__, xprt, xprt->state, xdrs);
		xdr_rdma_destroy(xdrs);
		return ENODEV;
	}

	/* buffer sizes MUST be page sized */
	xprt->sendsize = sendsize & ~(ps - 1);
	xprt->sendsize = xprt->sendsize >= ps ? xprt->sendsize : ps;
	xprt->recvsize = recvsize & ~(ps - 1);
	xprt->recvsize = xprt->recvsize >= ps ? xprt->recvsize : ps;

	/* pre-allocated buffer_total:
	 * the number of credits is irrelevant here.
	 * instead, allocate buffers to match the read/write contexts.
	 * more than one buffer can be chained to one ioq_uv head,
	 * but never need more ioq_uv heads than buffers.
	 */
	xprt->buffer_total = recvsize * xprt->xa->rq_depth
			   + sendsize * xprt->xa->sq_depth;

	xprt->buffer_aligned = mem_alloc_aligned(xprt->buffer_total, ps);
	if (xprt->buffer_aligned == NULL)
		goto err;

	__warnx(TIRPC_DEBUG_FLAG_RPC_RDMA,
		"%s() buffer_aligned at %p",
		__func__, xprt->buffer_aligned);

	/* register it in two chunks for read and write??? */
	xprt->mr = ibv_reg_mr(xprt->pd->pd, xprt->buffer_aligned,
				xprt->buffer_total,
				IBV_ACCESS_LOCAL_WRITE |
				IBV_ACCESS_REMOTE_WRITE |
				IBV_ACCESS_REMOTE_READ);

	poolq_head_setup(&xprt->inbufs.uvqh);
	xprt->inbufs.min_bsize = ps;
	xprt->inbufs.max_bsize = xprt->recvsize;

	poolq_head_setup(&xprt->outbufs.uvqh);
	xprt->outbufs.min_bsize = ps;
	xprt->outbufs.max_bsize = xprt->sendsize;

	/* modified local copy of operations */
	xdr_rdma_ops_recycle = xdr_ioq_ops;
	xdr_rdma_ops_recycle.x_destroy = xdr_rdma_post_recycle;

	/* Each pre-allocated buffer has a corresponding xdr_ioq_uv,
	 * stored on the pool queues.
	 */
	b = xprt->buffer_aligned;

	for (xprt->inbufs.uvqh.qcount = 0;
	     xprt->inbufs.uvqh.qcount < xprt->xa->rq_depth;
	     xprt->inbufs.uvqh.qcount++) {
		struct xdr_ioq_uv *data = xdr_ioq_uv_create(0, UIO_FLAG_BUFQ);

		data->v.vio_base =
		data->v.vio_head =
		data->v.vio_tail = b;
		data->v.vio_wrap = b + xprt->recvsize;
		data->u.uio_p1 = &xprt->inbufs.uvqh;
		data->u.uio_p2 = xprt->mr;
		TAILQ_INSERT_TAIL(&xprt->inbufs.uvqh.qh, &data->uvq, q);

		b += xprt->recvsize;
	}

	for (xprt->outbufs.uvqh.qcount = 0;
	     xprt->outbufs.uvqh.qcount < xprt->xa->sq_depth;
	     xprt->outbufs.uvqh.qcount++) {
		struct xdr_ioq_uv *data = xdr_ioq_uv_create(0, UIO_FLAG_BUFQ);

		data->v.vio_base =
		data->v.vio_head =
		data->v.vio_tail = b;
		data->v.vio_wrap = b + xprt->sendsize;
		data->u.uio_p1 = &xprt->outbufs.uvqh;
		data->u.uio_p2 = xprt->mr;
		TAILQ_INSERT_TAIL(&xprt->outbufs.uvqh.qh, &data->uvq, q);

		b += xprt->sendsize;
	}

	xdr_ioq_setup(&xprt->waitq);
	while (xprt->waitq.ioq_uv.uvqh.qcount < CALLQ_SIZE) {
		xdr_rdma_callq(xprt);
	}
	return 0;

err:
	__warnx(TIRPC_DEBUG_FLAG_ERROR,
		"%s() out of memory",
		__func__);
	xdr_rdma_destroy(xdrs);
	return ENOMEM;
}

/** xdr_rdma_clnt_call
 *
 * Client processes a call request
 *
 * @param[IN] xdrs	cm_data
 *
 * called by clnt_rdma_call()
 */
bool
xdr_rdma_clnt_call(XDR *xdrs, u_int32_t xid)
{
	RDMAXPRT *xprt;
	struct rpc_rdma_cbc *cbc = (struct rpc_rdma_cbc *)xdrs;

	if (!xdrs) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() no xdrs?",
			__func__);
		return (false);
	}
	xprt = x_xprt(xdrs);

	/* free old buffers */
#ifdef FIXME
		/* FIXME: only clears the ones associated with last request */
		for(i=0; i<xprt->credits; i++) {
			xprt->inbufs[i].len = 0;
			xprt->inbufs[i].next = NULL;
		}
#endif
	xdr_ioq_release(&cbc->holdq.ioq_uv.uvqh);
	xdr_rdma_callq(xprt);

	/* get new buffer */
#ifdef OLDCODE
	// Client encodes a call request
	case XDR_ENCODE:
		pthread_mutex_lock(&mi->cl.lock);
		mi->xdrbuf = mi->curbuf = xdrmsk_getfreebuf(mi->outbufs, mi);
#endif
	(void) xdr_ioq_uv_fetch(&cbc->workq, &xprt->outbufs.uvqh,
				"call buffer", 1, IOQ_FLAG_NONE);

#ifdef OLDCODE
		mi->pos = xdrs->x_base = (char*)mi->curbuf->data;
		xdrs->x_ops = ((unsigned long)xdrs->x_base & (sizeof(int32_t) - 1))
		    ? &xdrmsk_ops_unaligned : &xdrmsk_ops_aligned;
		pthread_mutex_unlock(&mi->cl.lock);
#endif

	xdr_ioq_reset(&cbc->workq, 0);
	return (true);
}

/** xdr_rdma_clnt_reply
 *
 * Client prepares for a reply
 *
 * potential output buffers are queued in workq.
 *
 * @param[IN] xdrs	cm_data
 *
 * called by clnt_rdma_call()
 */
bool
xdr_rdma_clnt_reply(XDR *xdrs, u_int32_t xid)
{
	RDMAXPRT *xprt;
	struct rpcrdma_write_array *reply_array;
	struct xdr_ioq_uv *work_uv;
	struct rpc_rdma_cbc *cbc = (struct rpc_rdma_cbc *)xdrs;

	if (!xdrs) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() no xdrs?",
			__func__);
		return (false);
	}
	xprt = x_xprt(xdrs);

	/* free old buffers */
	xdr_rdma_post_recv_cb(xprt, cbc); /* ??? */

#ifdef OLDCODE
	// Client decodes a reply buffer
	case XDR_DECODE:
		pthread_mutex_lock(&mi->cl.lock);
		mi->curbuf = xdrmsk_getxidbuf(mi->inbufs, mi, xid);
#endif
	work_uv = IOQ_(TAILQ_FIRST(&cbc->workq.ioq_uv.uvqh.qh));

	rpcrdma_dump_msg(work_uv, "replyhead", htonl(xid));

	reply_array = xdr_rdma_get_reply_array(work_uv->v.vio_head);
	if (reply_array->wc_discrim == 0) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() No reply/read array, failing miserably "
			"till writes/inlines are handled",
			__func__);
		return (false);
	} else {
#if 0
		int i = 0;

		prev_buf = NULL;
		for (i=0; i < ntohl(reply_array->wc_nchunks); i++) {
			/* FIXME: xdr_rdma_getaddrbuf hangs instead of
			 * failing if no match. add a zero timeout
			 * when implemented
			 */
			tmp_buf = xdr_rdma_getaddrbuf(mi->inbufs, mi,
				(uint8_t*) xdr_decode_hyper(&reply_array->wc_array[i].wc_target.rs_offset));

			/* rs_length < size if the protocol works out...
			 * FIXME: check anyway?
			 */
			tmp_buf->len = ntohl(reply_array->wc_array[i].wc_target.rs_length);

			rpcrdma_dump_msg(tmp_buf, "replybody", htonl(xid));

			if (prev_buf)
				prev_buf->next = tmp_buf;
			else
				mi->xdrbuf = tmp_buf;

			prev_buf = tmp_buf;
		}
#endif
	}

#ifdef OLDCODE
		xdrs->x_handy = mi->xdrbuf->len;
		mi->pos = xdrs->x_base = (char*)mi->xdrbuf->base;

		xdrs->x_ops = ((unsigned long)xdrs->x_base & (sizeof(int32_t) - 1))
		    ? &xdrmsk_ops_unaligned : &xdrmsk_ops_aligned;
#endif

	xdr_ioq_reset(&cbc->workq, 0);
	return (true);
}

/** xdr_rdma_svc_recv
 *
 * Server assembles a call request
 *
 * concatenates any rdma Read buffers for processing,
 * but clones call rdma header in place for future use.
 *
 * @param[IN] cbc	incoming request
 *			call request is in workq
 *
 * called by svc_rdma_recv()
 */
bool
xdr_rdma_svc_recv(struct rpc_rdma_cbc *cbc, u_int32_t xid)
{
	RDMAXPRT *xprt;
	struct rpcrdma_read_chunk *read_chunk;
	struct xdr_ioq_uv *head_uv;
	struct xdr_ioq_uv *work_uv;
	u_int s;

	if (!cbc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() no context?",
			__func__);
		return (false);
	}
	xprt = x_xprt(cbc->workq.xdrs);

	/* free old buffers (should do nothing) */
	xdr_ioq_release(&cbc->holdq.ioq_uv.uvqh);
	xdr_rdma_callq(xprt);

#ifdef OLDCODE
	// Server decodes a call request
	case XDR_DECODE:
		pthread_mutex_lock(&mi->cl.lock);
		mi->callbuf = mi->curbuf = xdrmsk_getusedbuf(mi->inbufs, mi);
		pthread_mutex_unlock(&mi->cl.lock);
#endif
	head_uv = IOQ_(TAILQ_FIRST(&cbc->workq.ioq_uv.uvqh.qh));

	rpcrdma_dump_msg(head_uv, "call", _md(head_uv->v.vio_head)->rm_xid);

	/* clone call head before processing */
	(head_uv->u.uio_references)++;
	cbc->call_uv.u.uio_refer = &head_uv->u;
	cbc->call_uv.v = head_uv->v;

	/* swap calling message from workq to holdq */
	TAILQ_CONCAT(&cbc->holdq.ioq_uv.uvqh.qh, &cbc->workq.ioq_uv.uvqh.qh, q);
	cbc->holdq.ioq_uv.uvqh.qcount = cbc->workq.ioq_uv.uvqh.qcount;
	cbc->workq.ioq_uv.uvqh.qcount = 0;

#ifdef OLDCODE
		read_chunk = rpcrdma_get_read_list((struct rpcrdma_msg*)mi->curbuf->data);
		prev_buf = NULL;
#endif
	read_chunk = xdr_rdma_get_read_list(cbc->call_uv.v.vio_head);
	while (read_chunk->rc_discrim != 0) {
		work_uv = IOQ_(xdr_ioq_uv_fetch(&cbc->workq, &xprt->inbufs.uvqh,
						"read buffer", 1,
						IOQ_FLAG_NONE));

		/* pre-set the len to the requested rs_length */
		s = ntohl(read_chunk->rc_target.rs_length);
		if (s > xprt->recvsize) {
			__warnx(TIRPC_DEBUG_FLAG_XDR,
				"%s() requested read %u is too long",
				__func__, s);
			s = xprt->recvsize;
		}
		work_uv->v.vio_head = work_uv->v.vio_base;
		work_uv->v.vio_tail = work_uv->v.vio_base + s;
/*		work_uv->v.vio_wrap = work_uv->v.vio_head + xprt->recvsize; */

		/* FIXME: get them only when needed by xdr_ioq_uv_next,
		 * or post all the reads and wait only at the end...
		 */
		xdr_rdma_wait_read_n(xprt, cbc, &read_chunk->rc_target); /* 1 */

		rpcrdma_dump_msg(work_uv, "readdata",
				 _md(work_uv->v.vio_head)->rm_xid);

#ifdef OLDCODE
			if (prev_buf)
				prev_buf->next = tmp_buf;
			else
				mi->curbuf->next = tmp_buf;
			prev_buf = tmp_buf;
			read_chunk++;
#endif

		/* concatenate any additional buffers after the calling message,
		 * assuming there is more call data in the calling buffer.
		 */
		TAILQ_CONCAT(&cbc->holdq.ioq_uv.uvqh.qh,
			     &cbc->workq.ioq_uv.uvqh.qh, q);
		cbc->holdq.ioq_uv.uvqh.qcount += cbc->workq.ioq_uv.uvqh.qcount;
		cbc->workq.ioq_uv.uvqh.qcount = 0;
		read_chunk++;
	}

#ifdef OLDCODE
		xdrs->x_ops = ((unsigned long)xdrs->x_base & (sizeof(int32_t) - 1))
		    ? &xdrmsk_ops_unaligned : &xdrmsk_ops_aligned;
		break;
#endif
	/* skip past the header for the calling buffer */
	xdr_ioq_reset(&cbc->holdq,
			xdr_rdma_header_length(_md(cbc->call_uv.v.vio_head)));
	return (true);
}

/** xdr_rdma_svc_reply
 *
 * Server prepares for a reply
 *
 * potential output buffers are queued in workq.
 *
 * @param[IN] cbc	incoming request
 *			call request is in holdq
 *
 * called by svc_rdma_reply()
 */
bool
xdr_rdma_svc_reply(struct rpc_rdma_cbc *cbc, u_int32_t xid)
{
	RDMAXPRT *xprt;
	struct rpcrdma_write_array *call_array;
	struct poolq_entry *have;
	int n;

	if (!cbc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() no context?",
			__func__);
		return (false);
	}
	xprt = x_xprt(cbc->workq.xdrs);

	/* release previous output buffers (should do nothing) */
	xdr_ioq_release(&cbc->workq.ioq_uv.uvqh);

#ifdef OLDCODE
	// Server encodes a reply
	case XDR_ENCODE:
		call_array = rpcrdma_get_reply_array((struct rpcrdma_msg*)mi->callbuf->data);
#endif
	call_array = xdr_rdma_get_reply_array(cbc->call_uv.v.vio_head);

	if (unlikely(call_array->wc_discrim == 0)) {
#ifdef OLDCODE
			// no reply array to write to, replying inline an' hope it works (OK on RPC/RDMA Read)
			mi->curbuf = xdrmsk_getfreebuf(mi->outbufs, mi);
#endif
		/* no reply array to write, replying inline an' hope
		 * (OK on RPC/RDMA Read)
		 */
		have = xdr_ioq_uv_fetch(&cbc->workq, &xprt->outbufs.uvqh,
					"reply buffer", 1, IOQ_FLAG_NONE);

		/* restore size after previous usage */
		IOQ_(have)->v.vio_wrap = IOQ_(have)->v.vio_base
					+ xprt->sendsize;
	} else {
		struct rpcrdma_write_chunk *wc = (struct rpcrdma_write_chunk *)
						call_array->wc_array;

		/* ensure never asking for more buffers than available */
		n = ntohl(call_array->wc_nchunks);
		if (n > xprt->xa->credits) {
			/* buffers allowed in flight */
			n = xprt->xa->credits;
		}
		/* ensure we can get all of our buffers without deadlock
		 * (wait for them all to be appended)
		 */
		(void)xdr_ioq_uv_fetch(&cbc->workq, &xprt->outbufs.uvqh,
					"reply buffers", n, IOQ_FLAG_NONE);

		TAILQ_FOREACH(have, &cbc->workq.ioq_uv.uvqh.qh, q) {
			u_int s = ntohl((wc++)->wc_target.rs_length);

			/* pre-set the size to the requested rs_length */
			if (s > xprt->sendsize) {
				__warnx(TIRPC_DEBUG_FLAG_XDR,
					"%s() requested send %u is too long",
					__func__, s);
				s = xprt->sendsize;
			}
			IOQ_(have)->v.vio_head =
			IOQ_(have)->v.vio_tail = IOQ_(have)->v.vio_base;
			IOQ_(have)->v.vio_wrap = IOQ_(have)->v.vio_base + s;
		}
	}

#ifdef OLDCODE
		xdrs->x_ops = ((unsigned long)xdrs->x_base & (sizeof(int32_t) - 1))
		    ? &xdrmsk_ops_unaligned : &xdrmsk_ops_aligned;
		break;
#endif
	xdr_ioq_reset(&cbc->workq, 0);
	return (true);
}

/** xdr_rdma_clnt_flushout
 *
 * @param[IN] xdrs	combined callback context
 *
 * @return true is message sent, false otherwise
 *
 * called by clnt_rdma_call()
 */
bool
xdr_rdma_clnt_flushout(XDR * xdrs)
{
/* FIXME: decide how many buffers we use in argument!!!!!! */
#define num_chunks (xprt->xa->credits - 1)

	RDMAXPRT *xprt;
	struct rpc_msg *msg;
	struct rpcrdma_msg *rmsg;
	struct rpcrdma_write_array *w_array;
	struct xdr_ioq_uv *head_uv;
	struct xdr_ioq_uv *work_uv;
	struct poolq_entry *have;
	struct rpc_rdma_cbc *cbc = (struct rpc_rdma_cbc *)xdrs;
	int i = 0;

	if (!xdrs) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() no xdrs?",
			__func__);
		return (false);
	}
	xprt = x_xprt(xdrs);

	work_uv = IOQ_(TAILQ_FIRST(&cbc->workq.ioq_uv.uvqh.qh));
	msg = (struct rpc_msg *)(work_uv->v.vio_head);
	xdr_tail_update(cbc->workq.xdrs);

	switch(ntohl(msg->rm_direction)) {
	    case CALL:
		/* good to go */
		break;
	    case REPLY:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() nothing to send on REPLY (%u)",
			__func__, ntohl(msg->rm_direction));
		return (true);
	    default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() bad rm_direction (%u)",
			__func__, ntohl(msg->rm_direction));
		return (false);
	}

	cbc->workq.ioq_uv.uvq_fetch = xdr_ioq_uv_fetch_nothing;

	head_uv = IOQ_(xdr_ioq_uv_fetch(&cbc->workq, &xprt->outbufs.uvqh,
					"c_head buffer", 1, IOQ_FLAG_NONE));

	(void)xdr_ioq_uv_fetch(&cbc->holdq, &xprt->inbufs.uvqh,
				"call buffers", num_chunks, IOQ_FLAG_NONE);

	rmsg = _md(head_uv->v.vio_head);
	rmsg->rm_xid = msg->rm_xid;
	rmsg->rm_vers = htonl(1);
	rmsg->rm_credit = htonl(xprt->xa->credits - cbc->holdq.ioq_uv.uvqh.qcount);
	rmsg->rm_type = htonl(RDMA_MSG);

		/* no read, write chunks. */
	rmsg->rm_body.rm_chunks[0] = 0; /* htonl(0); */
	rmsg->rm_body.rm_chunks[1] = 0; /* htonl(0); */

		/* reply chunk */
	w_array = (_wa_t *)&rmsg->rm_body.rm_chunks[2];
	w_array->wc_discrim = htonl(1);
	w_array->wc_nchunks = htonl(num_chunks);

	TAILQ_FOREACH(have, &cbc->holdq.ioq_uv.uvqh.qh, q) {
		struct rpcrdma_segment *w_seg =
			&w_array->wc_array[i++].wc_target;
		uint32_t length = ioquv_length(IOQ_(have));

		w_seg->rs_handle = htonl(xprt->mr->rkey);
		w_seg->rs_length = htonl(length);
		xdr_encode_hyper((uint32_t*)&w_seg->rs_offset,
				 (uintptr_t)IOQ_(have)->v.vio_head);
	}

	head_uv->v.vio_tail = head_uv->v.vio_head
				+ xdr_rdma_header_length(rmsg);

	rpcrdma_dump_msg(head_uv, "clnthead", msg->rm_xid);
	rpcrdma_dump_msg(work_uv, "clntcall", msg->rm_xid);

		/* actual send, callback will take care of cleanup */
	xdr_rdma_post_send_cb(xprt, cbc); /* 2 */
	return (true);
}

/** xdr_rdma_svc_flushout
 *
 * @param[IN] cbc	combined callback context
 *
 * called by svc_rdma_reply()
 */
bool
xdr_rdma_svc_flushout(struct rpc_rdma_cbc *cbc)
{
	RDMAXPRT *xprt;
	struct rpc_msg *msg;
	struct rpcrdma_msg *rmsg;
	struct rpcrdma_write_array *w_array;
	struct rpcrdma_write_array *call_array;
	struct xdr_ioq_uv *head_uv;
	struct xdr_ioq_uv *work_uv;
	struct poolq_entry *have;
	struct poolq_entry *safe;
	int i = 0;

	if (!cbc) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() no context?",
			__func__);
		return (false);
	}
	xprt = x_xprt(cbc->workq.xdrs);

	/* release previous input buffers (head will be retained) */
	xdr_ioq_release(&cbc->holdq.ioq_uv.uvqh);

	work_uv = IOQ_(TAILQ_FIRST(&cbc->workq.ioq_uv.uvqh.qh));
	msg = (struct rpc_msg *)(work_uv->v.vio_head);
	/* work_uv->v.vio_tail has been set by xdr_tail_update() */

	switch(ntohl(msg->rm_direction)) {
	    case CALL:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() nothing to send on CALL (%u)",
			__func__, ntohl(msg->rm_direction));
		return (true);
	    case REPLY:
		/* good to go */
		break;
	    default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s() bad rm_direction (%u)",
			__func__, ntohl(msg->rm_direction));
		return (false);
	}

	/* usurp the holdq for the head, move to workq later */
	head_uv = IOQ_(xdr_ioq_uv_fetch(&cbc->holdq, &xprt->outbufs.uvqh,
					"s_head buffer", 1, IOQ_FLAG_NONE));

	/* entry was already added directly to the queue */
	head_uv->v.vio_head = head_uv->v.vio_base;
	/* restore size after previous usage */
	head_uv->v.vio_wrap = head_uv->v.vio_base + xprt->sendsize;

		/* CHECKS HERE */

	call_array = xdr_rdma_get_reply_array(cbc->call_uv.v.vio_head);

	/* build the header that goes with the data */
	rmsg = _md(head_uv->v.vio_head);
	rmsg->rm_xid = msg->rm_xid;
	/* TODO: check it matches mi->hdrbuf xid */
	rmsg->rm_vers = htonl(1);
	rmsg->rm_credit = htonl(xprt->xa->credits - cbc->workq.ioq_uv.uvqh.qcount);

		/* no read, write chunks. */
	rmsg->rm_body.rm_chunks[0] = 0; /* htonl(0); */
	rmsg->rm_body.rm_chunks[1] = 0; /* htonl(0); */

	if (call_array->wc_discrim == 0) {
		rmsg->rm_type = htonl(RDMA_MSG);

			/* no reply chunk either */
		rmsg->rm_body.rm_chunks[2] = 0; /* htonl(0); */

		head_uv->v.vio_tail = head_uv->v.vio_head
					+ xdr_rdma_header_length(rmsg);

		rpcrdma_dump_msg(head_uv, "replyhead", msg->rm_xid);
		rpcrdma_dump_msg(work_uv, "replybody", msg->rm_xid);

			/* actual send, callback will take care of cleanup */
		TAILQ_REMOVE(&cbc->holdq.ioq_uv.uvqh.qh, &head_uv->uvq, q);
		(cbc->holdq.ioq_uv.uvqh.qcount)--;
		(cbc->workq.ioq_uv.uvqh.qcount)++;
		TAILQ_INSERT_HEAD(&cbc->workq.ioq_uv.uvqh.qh, &head_uv->uvq, q);

		/* TODO: make it work with not just one outbuf,
		 * but get more outbufs as needed in
		 * xdr_rdma_getnextbuf? add something in mi instead
		 * of checking call_array->wc_discrim everytime...
		 */
		xdr_rdma_post_send_cb(xprt, cbc); /* 2 */
	} else {
		rmsg->rm_type = htonl(RDMA_NOMSG);

			/* reply chunk */
		w_array = (_wa_t *)&rmsg->rm_body.rm_chunks[2];
		w_array->wc_discrim = htonl(1);

		TAILQ_FOREACH_SAFE(have, &cbc->workq.ioq_uv.uvqh.qh, q, safe) {
			struct poolq_entry *pqe;
			struct rpc_rdma_cbc *wait;
			struct rpcrdma_segment *c_seg =
				&call_array->wc_array[i].wc_target;
			struct rpcrdma_segment *w_seg =
				&w_array->wc_array[i++].wc_target;
			uint32_t length = ioquv_length(IOQ_(have));

			/* restore size after previous usage */
			IOQ_(have)->v.vio_wrap = IOQ_(have)->v.vio_base
						+ xprt->sendsize;

			TAILQ_REMOVE(&cbc->workq.ioq_uv.uvqh.qh, have, q);
			(cbc->workq.ioq_uv.uvqh.qcount)--;

			/* This is NOT checked in xdr_rdma_post_write???
			if (length < ntohl(c_seg->rs_length))
				return FALSE;
			 */
			if (!length) {
				/* buffer unused. recycle. */
				xdr_ioq_uv_release(IOQ_(have));
				i--;
				continue;
			}
			rpcrdma_dump_msg(IOQ_(have), "writedata", msg->rm_xid);

			pqe = xdr_ioq_uv_fetch(&xprt->waitq, &xprt->cbqh,
					"write context", 1, IOQ_FLAG_NONE);
			wait = (struct rpc_rdma_cbc *)(_IOQ(pqe));

			/** @todo: if remote addr + len = next remote
				addr and send in a single write */
			(wait->workq.ioq_uv.uvqh.qcount)++;
			TAILQ_INSERT_TAIL(&wait->workq.ioq_uv.uvqh.qh, have, q);

			xdr_rdma_post_write_cb(xprt, wait, c_seg); /* 1 */

			w_seg->rs_handle = c_seg->rs_handle;
			w_seg->rs_length = htonl(length);
			w_seg->rs_offset = c_seg->rs_offset;
		}
		w_array->wc_nchunks = htonl(i);

		head_uv->v.vio_tail = head_uv->v.vio_head
					+ xdr_rdma_header_length(rmsg);

		rpcrdma_dump_msg(head_uv, "writehead", msg->rm_xid);

			/* actual send, callback will take care of cleanup */
		TAILQ_REMOVE(&cbc->holdq.ioq_uv.uvqh.qh, &head_uv->uvq, q);
		(cbc->holdq.ioq_uv.uvqh.qcount)--;
		(cbc->workq.ioq_uv.uvqh.qcount)++;
		TAILQ_INSERT_HEAD(&cbc->workq.ioq_uv.uvqh.qh, &head_uv->uvq, q);
		xdr_rdma_post_send_cb(xprt, cbc); /* 1 */
	}

		/* free the old inbuf we only kept for header, and repost it. */
	head_uv = IOQU(cbc->call_uv.u.uio_refer);
	cbc->call_uv.u.uio_refer = NULL;
	xdr_ioq_uv_release(head_uv);
	return (true);
}

#ifdef FIXMEREPLACED
static u_int
xdrmsk_getpos(XDR *xdrs)
{
	/* XXX w/64-bit pointers, u_int not enough! */
	return (u_int)((u_long)priv(xdrs)->pos - (u_long)xdrs->x_base);
}

/* FIXME: Completely broken with buffer cut in parts... */
static bool
xdrmsk_setpos(XDR *xdrs, u_int pos)
{
	char *newaddr = xdrs->x_base + pos;
	char *lastaddr = (char *)priv(xdrs)->pos + xdrs->x_handy;

	if (newaddr > lastaddr)
		return (FALSE);
	priv(xdrs)->pos = newaddr;
	xdrs->x_handy = (u_int)(lastaddr - newaddr); /* XXX sizeof(u_int) <? sizeof(ptrdiff_t) */
	return (TRUE);
}

static bool
xdrmsk_getnextbuf(XDR *xdrs)
{
	msk_data_t *data;

	if (priv(xdrs)->xdrbuf == NULL)
		return FALSE;

	data = priv(xdrs)->xdrbuf;

	if (data->next == NULL)
		return FALSE;

	data->size = xdrmsk_getpos(xdrs);

	data = priv(xdrs)->xdrbuf = priv(xdrs)->xdrbuf->next;
	xdrs->x_handy = data->size;
	xdrs->x_base = priv(xdrs)->pos = (char*)data->data;
	xdrs->x_ops = ((unsigned long)xdrs->x_base & (sizeof(int32_t) - 1))
	    ? &xdrmsk_ops_unaligned : &xdrmsk_ops_aligned;

	return TRUE;
}

static uint32_t
xdrmsk_getsizeleft(XDR *xdrs)
{
	uint32_t count = xdrs->x_handy;
	msk_data_t *data = priv(xdrs)->xdrbuf;

	while (data->next) {
		data=data->next;
		count += data->size;
	}

	return count;
}

static bool
xdrmsk_getlong_aligned(XDR *xdrs, long *lp)
{

	if (xdrs->x_handy < sizeof(int32_t)) {
		if (xdrmsk_getnextbuf(xdrs))
			return xdrs->x_ops->x_getlong(xdrs, lp);
		else
			return (FALSE);
	}
	xdrs->x_handy -= sizeof(int32_t);
	*lp = ntohl(*(u_int32_t *)priv(xdrs)->pos);
	priv(xdrs)->pos = (char *)priv(xdrs)->pos + sizeof(int32_t);
	return (TRUE);
}

static bool
xdrmsk_putlong_aligned(XDR *xdrs, const long *lp)
{

	if (xdrs->x_handy < sizeof(int32_t)) {
		if (xdrmsk_getnextbuf(xdrs))
			return xdrs->x_ops->x_putlong(xdrs, lp);
		else
			return (FALSE);
	}
	xdrs->x_handy -= sizeof(int32_t);
	*(u_int32_t *)priv(xdrs)->pos = htonl((u_int32_t)*lp);
	priv(xdrs)->pos = (char *)priv(xdrs)->pos + sizeof(int32_t);
	return (TRUE);
}

static bool
xdrmsk_getlong_unaligned(XDR *xdrs, long *lp)
{
	u_int32_t l;

	if (xdrs->x_handy < sizeof(int32_t)) {
		if (xdrmsk_getnextbuf(xdrs))
			return xdrs->x_ops->x_getlong(xdrs, lp);
		else
			return (FALSE);
	}
	xdrs->x_handy -= sizeof(int32_t);
	memmove(&l, priv(xdrs)->pos, sizeof(int32_t));
	*lp = ntohl(l);
	priv(xdrs)->pos = (char *)priv(xdrs)->pos + sizeof(int32_t);
	return (TRUE);
}

static bool
xdrmsk_putlong_unaligned(XDR *xdrs, const long *lp)
{
	u_int32_t l;

	if (xdrs->x_handy < sizeof(int32_t)) {
		if (xdrmsk_getnextbuf(xdrs))
			return xdrs->x_ops->x_putlong(xdrs, lp);
		else
			return (FALSE);
	}
	xdrs->x_handy -= sizeof(int32_t);
	l = htonl((u_int32_t)*lp);
	memmove(priv(xdrs)->pos, &l, sizeof(int32_t));
	priv(xdrs)->pos = (char *)priv(xdrs)->pos + sizeof(int32_t);
	return (TRUE);
}

static bool
xdrmsk_getbytes(XDR *xdrs, char *addr, u_int len)
{
	u_int size;

	if (xdrmsk_getsizeleft(xdrs) < len)
			return (FALSE);

	while (len > 0) {
		if (xdrs->x_handy == 0)
			if (xdrmsk_getnextbuf(xdrs) == FALSE)
				return (FALSE);
		size = MIN(len, xdrs->x_handy);
		memmove(addr, priv(xdrs)->pos, size);
		addr += size;
		len -= size;
		xdrs->x_handy -= size;
		priv(xdrs)->pos = (char *)priv(xdrs)->pos + size;
	}
	return (TRUE);
}

static bool
xdrmsk_putbytes(XDR *xdrs, const char *addr, u_int len)
{
	u_int size;

	if (xdrmsk_getsizeleft(xdrs) < len)
		return (FALSE);

	while (len > 0) {
		if (xdrs->x_handy == 0)
			if (xdrmsk_getnextbuf(xdrs) == FALSE)
				return (FALSE);
		size = MIN(len, xdrs->x_handy);
		memmove(priv(xdrs)->pos, addr, size);
		addr += size;
		len -= size;
		xdrs->x_handy -= size;
		priv(xdrs)->pos = (char *)priv(xdrs)->pos + size;
	}
	return (TRUE);
}

static int32_t *
xdrmsk_inline_aligned(XDR *xdrs, u_int len)
{
	int32_t *buf = NULL;

	if (xdrs->x_handy >= len) {
		xdrs->x_handy -= len;
		buf = (int32_t *)priv(xdrs)->pos;
		priv(xdrs)->pos = (char *)priv(xdrs)->pos + len;
	} else {
		if (xdrmsk_getnextbuf(xdrs))
			return xdrs->x_ops->x_inline(xdrs, len);
		else
			return NULL;
	}
	return (buf);
}

/* ARGSUSED */
static int32_t *
xdrmsk_inline_unaligned(XDR *xdrs, u_int len)
{
	return (NULL);
}
#endif /* FIXMEREPLACED */
