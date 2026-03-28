/*
 * src/goc_io.c — Async I/O wrappers for libgoc
 *
 * Implements the channel-returning wrappers declared in include/goc_io.h.
 *
 * Thread-safety strategy
 * ----------------------
 * File-system (uv_fs_*) and DNS (uv_getaddrinfo, uv_getnameinfo) operations
 * are submitted directly from any thread because libuv routes them through
 * its internal worker-thread pool with proper locking.
 *
 * Stream and UDP handle operations (uv_read_start, uv_write, uv_tcp_connect,
 * uv_pipe_connect, uv_shutdown, uv_udp_send, uv_udp_recv_start and the
 * matching stop functions) touch libuv handle internals that are not
 * thread-safe.  These are dispatched to the event loop thread via a
 * one-shot uv_async_t, following the same pattern used by goc_timeout.
 *
 * Result delivery
 * ---------------
 * All one-shot callbacks deliver their result via goc_put_cb() with a
 * close_on_put completion callback.  goc_put_cb() is non-blocking: it posts
 * the put to the MPSC queue for the loop thread to process.  The loop thread
 * delivers the value to any parked fiber taker (or buffers it) and then
 * fires close_on_put, which calls goc_close().  This ordering guarantees
 * that the channel is never closed before the value is delivered, regardless
 * of which thread runs the I/O callback.
 *
 * Scalar vs. struct results
 * -------------------------
 * Operations whose only result is a scalar (status code, byte count, file
 * descriptor) encode the value as (void*)(intptr_t)value in the channel.
 * Operations that need composite results allocate a GC-managed struct and
 * deliver its pointer through the channel.
 *
 * GC safety
 * ---------
 * All context and dispatch structs are GC-allocated via goc_malloc.  The GC
 * can scan them and see the goc_chan* fields they contain, keeping channels
 * alive.  Context/dispatch structs are registered with gc_handle_register
 * before being handed to libuv so they are not collected while libuv holds a
 * reference to them.  Stream/UDP recv context structs (goc_stream_ctx_t,
 * goc_udp_recv_ctx_t) are not registered separately — they are kept alive
 * transitively because they are reachable from handle->data, and the user
 * handle is registered via goc_io_handle_register.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <uv.h>
#include <gc.h>
#include "../include/goc_io.h"
#include "internal.h"

/* =========================================================================
 * Shared helpers
 * ====================================================================== */

/* Callback used as uv_close completion for GC-allocated dispatch structs. */
static void unregister_io_handle(uv_handle_t* h)
{
    gc_handle_unregister(h);
}

static void dispatch_async_or_abort(uv_async_t* async,
                                    uv_async_cb cb,
                                    const char* op_name)
{
    int rc = uv_async_init(g_loop, async, cb);
    if (rc < 0) {
        fprintf(stderr, "libgoc: uv_async_init failed in %s: %s\n",
                op_name, uv_strerror(rc));
        abort();
    }

    rc = uv_async_send(async);
    if (rc < 0) {
        fprintf(stderr, "libgoc: uv_async_send failed in %s: %s\n",
                op_name, uv_strerror(rc));
        abort();
    }
}

/* =========================================================================
 * 3. File System Operations
 *
 * uv_fs_* functions are safe to call from any thread; no async bridge needed.
 * All FS context structs embed uv_fs_t as the first member so the context
 * pointer can be recovered from the req pointer inside callbacks.
 * ====================================================================== */

typedef struct {
    uv_fs_t  req;   /* MUST be first member */
    goc_chan* ch;
} goc_fs_ctx_t;

/* Helper: encode a scalar result as void* for channel delivery. */
#define SCALAR(v)  ((void*)(intptr_t)(v))
/* Helper: decode scalar from channel take. */
#define INT_VAL(v) ((int)(intptr_t)(v)->val)

/* put_cb that closes the channel once the value has been delivered. */
static void close_on_put(goc_status_t ok, void* ud)
{
    (void)ok;
    goc_close((goc_chan*)ud);
}

/* -------------------------------------------------------------------------
 * goc_io_fs_open
 * ---------------------------------------------------------------------- */

static void on_fs_open(uv_fs_t* req)
{
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)req;
    uv_file fd = (uv_file)req->result;
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(fd), close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_open(const char* path, int flags, int mode)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_open(g_loop, &ctx->req, path, flags, mode, on_fs_open);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_fs_close
 * ---------------------------------------------------------------------- */

static void on_fs_close(uv_fs_t* req)
{
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)req;
    int result = (int)req->result;
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(result), close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_close(uv_file file)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_close(g_loop, &ctx->req, file, on_fs_close);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_fs_read
 * ---------------------------------------------------------------------- */

static void on_fs_read(uv_fs_t* req)
{
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)req;
    ssize_t result = (ssize_t)req->result;
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(result), close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_read(uv_file file,
                            const uv_buf_t bufs[], unsigned int nbufs,
                            int64_t offset)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_read(g_loop, &ctx->req, file, bufs, nbufs, offset,
                        on_fs_read);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_fs_write
 * ---------------------------------------------------------------------- */

static void on_fs_write(uv_fs_t* req)
{
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)req;
    ssize_t result = (ssize_t)req->result;
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(result), close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_write(uv_file file,
                             const uv_buf_t bufs[], unsigned int nbufs,
                             int64_t offset)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_write(g_loop, &ctx->req, file, bufs, nbufs, offset,
                         on_fs_write);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_fs_unlink
 * ---------------------------------------------------------------------- */

static void on_fs_unlink(uv_fs_t* req)
{
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)req;
    int result = (int)req->result;
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(result), close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_unlink(const char* path)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_unlink(g_loop, &ctx->req, path, on_fs_unlink);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_fs_stat
 * ---------------------------------------------------------------------- */

static void on_fs_stat(uv_fs_t* req)
{
    goc_fs_ctx_t*     ctx = (goc_fs_ctx_t*)req;
    goc_io_fs_stat_t* res = (goc_io_fs_stat_t*)goc_malloc(sizeof(goc_io_fs_stat_t));
    res->ok = (req->result == 0) ? GOC_IO_OK : GOC_IO_ERR;
    if (req->result == 0)
        res->statbuf = req->statbuf;
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, res, close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_stat(const char* path)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_stat(g_loop, &ctx->req, path, on_fs_stat);
    if (rc < 0) {
        goc_io_fs_stat_t* res = (goc_io_fs_stat_t*)goc_malloc(sizeof(goc_io_fs_stat_t));
        res->ok = GOC_IO_ERR;
        goc_put_cb(ch, res, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_fs_rename
 * ---------------------------------------------------------------------- */

static void on_fs_rename(uv_fs_t* req)
{
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)req;
    int result = (int)req->result;
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(result), close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_rename(const char* path, const char* new_path)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_rename(g_loop, &ctx->req, path, new_path, on_fs_rename);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_fs_sendfile
 * ---------------------------------------------------------------------- */

static void on_fs_sendfile(uv_fs_t* req)
{
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)req;
    ssize_t result = (ssize_t)req->result;
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(result), close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_sendfile(uv_file out_fd, uv_file in_fd,
                                int64_t in_offset, size_t length)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_sendfile(g_loop, &ctx->req, out_fd, in_fd, in_offset,
                            length, on_fs_sendfile);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}


/* =========================================================================
 * 4. DNS & Resolution
 *
 * uv_getaddrinfo and uv_getnameinfo are safe to call from any thread.
 * ====================================================================== */

/* -------------------------------------------------------------------------
 * goc_io_getaddrinfo
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_getaddrinfo_t req;   /* MUST be first member */
    goc_chan*        ch;
} goc_getaddrinfo_ctx_t;

static void on_getaddrinfo(uv_getaddrinfo_t* req, int status,
                           struct addrinfo* res)
{
    goc_getaddrinfo_ctx_t* ctx = (goc_getaddrinfo_ctx_t*)req;
    goc_io_getaddrinfo_t*  r   = (goc_io_getaddrinfo_t*)goc_malloc(
                                     sizeof(goc_io_getaddrinfo_t));
    r->ok  = (status == 0) ? GOC_IO_OK : GOC_IO_ERR;
    r->res = res;
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, r, close_on_put, ctx->ch);
}

goc_chan* goc_io_getaddrinfo(const char* node, const char* service,
                                const struct addrinfo* hints)
{
    goc_chan*              ch  = goc_chan_make(1);
    goc_getaddrinfo_ctx_t* ctx = (goc_getaddrinfo_ctx_t*)goc_malloc(
                                     sizeof(goc_getaddrinfo_ctx_t));
    ctx->ch = ch;
    int rc = uv_getaddrinfo(g_loop, &ctx->req, on_getaddrinfo,
                            node, service, hints);
    if (rc < 0) {
        goc_io_getaddrinfo_t* r = (goc_io_getaddrinfo_t*)goc_malloc(
                                   sizeof(goc_io_getaddrinfo_t));
        r->ok  = GOC_IO_ERR;
        r->res = NULL;
        goc_put_cb(ch, r, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_getnameinfo
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_getnameinfo_t req;   /* MUST be first member */
    goc_chan*        ch;
} goc_getnameinfo_ctx_t;

static void on_getnameinfo(uv_getnameinfo_t* req, int status,
                           const char* hostname, const char* service)
{
    goc_getnameinfo_ctx_t* ctx = (goc_getnameinfo_ctx_t*)req;
    goc_io_getnameinfo_t*  r   = (goc_io_getnameinfo_t*)goc_malloc(
                                     sizeof(goc_io_getnameinfo_t));
    r->ok = (status == 0) ? GOC_IO_OK : GOC_IO_ERR;
    if (hostname)
        strncpy(r->hostname, hostname, sizeof(r->hostname) - 1);
    else
        r->hostname[0] = '\0';
    if (service)
        strncpy(r->service, service, sizeof(r->service) - 1);
    else
        r->service[0] = '\0';
    r->hostname[sizeof(r->hostname) - 1] = '\0';
    r->service[sizeof(r->service)  - 1] = '\0';
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, r, close_on_put, ctx->ch);
}

goc_chan* goc_io_getnameinfo(const struct sockaddr* addr, int flags)
{
    goc_chan*              ch  = goc_chan_make(1);
    goc_getnameinfo_ctx_t* ctx = (goc_getnameinfo_ctx_t*)goc_malloc(
                                     sizeof(goc_getnameinfo_ctx_t));
    ctx->ch = ch;
    int rc = uv_getnameinfo(g_loop, &ctx->req, on_getnameinfo, addr, flags);
    if (rc < 0) {
        goc_io_getnameinfo_t* r = (goc_io_getnameinfo_t*)goc_malloc(
                                   sizeof(goc_io_getnameinfo_t));
        r->ok         = GOC_IO_ERR;
        r->hostname[0] = '\0';
        r->service[0]  = '\0';
        goc_put_cb(ch, r, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}


/* =========================================================================
 * 1. Stream I/O  (TCP, Pipes, TTY)
 *
 * Stream handle operations are NOT thread-safe.  They are dispatched to the
 * event loop thread via a one-shot uv_async_t bridge.
 *
 * The streaming read and stop operations store a context pointer in
 * handle->data.  The caller must not use handle->data for other purposes
 * while a goc_io_read_start() / goc_io_read_stop() pair is active on that handle.
 * ====================================================================== */

/* -------------------------------------------------------------------------
 * Common alloc callback used by both stream read and UDP recv.
 *
 * malloc is used here (not goc_malloc) because libuv writes into buf->base
 * and holds a reference to it until the read/recv callback fires.  Since
 * libuv's internal state is not GC-visible, using goc_malloc would risk
 * premature collection of the buffer between alloc and the data callback.
 * The buffer is copied into a GC-managed uv_buf_t inside on_read_cb and
 * on_udp_recv_cb, and the malloc'd original is freed there.
 * ---------------------------------------------------------------------- */

static void goc_alloc_cb(uv_handle_t* handle, size_t suggested_size,
                         uv_buf_t* buf)
{
    (void)handle;
    buf->base = (char*)malloc(suggested_size);
    buf->len  = buf->base ? suggested_size : 0;
}

/* -------------------------------------------------------------------------
 * goc_io_read_start
 * ---------------------------------------------------------------------- */

typedef struct {
    goc_chan* ch;
} goc_stream_ctx_t;

static void on_read_cb(uv_stream_t* stream, ssize_t nread,
                       const uv_buf_t* buf)
{
    goc_stream_ctx_t* ctx = (goc_stream_ctx_t*)stream->data;

    if (nread == 0) {
        /* EAGAIN / EWOULDBLOCK — no data right now; free the buffer. */
        free(buf->base);
        return;
    }

    goc_io_read_t* res = (goc_io_read_t*)goc_malloc(sizeof(goc_io_read_t));
    res->nread = nread;

    if (nread > 0) {
        /* Normal data: copy into GC-managed buffer and free the malloc'd one. */
        uv_buf_t* gc_buf = (uv_buf_t*)goc_malloc(sizeof(uv_buf_t));
        gc_buf->base = (char*)goc_malloc((size_t)nread);
        gc_buf->len  = (size_t)nread;
        memcpy(gc_buf->base, buf->base, (size_t)nread);
        free(buf->base);
        res->buf = gc_buf;
        goc_put_cb(ctx->ch, res, NULL, NULL);
        return;
    }

    /* nread < 0: EOF or error.  Free the unused malloc'd buffer, deliver
     * final result, close channel. */
    free(buf->base);
    res->buf = NULL;
    goc_put_cb(ctx->ch, res, NULL, NULL);
    goc_close(ctx->ch);
    stream->data = NULL;
}

typedef struct {
    uv_async_t   async;   /* MUST be first member */
    uv_stream_t* stream;
    goc_chan*    ch;
} goc_read_start_dispatch_t;

static void on_read_start_dispatch(uv_async_t* h)
{
    goc_read_start_dispatch_t* d = (goc_read_start_dispatch_t*)h;

    goc_stream_ctx_t* ctx = (goc_stream_ctx_t*)goc_malloc(sizeof(goc_stream_ctx_t));
    ctx->ch        = d->ch;
    d->stream->data = ctx;

    int rc = uv_read_start(d->stream, goc_alloc_cb, on_read_cb);
    if (rc < 0) {
        /* Failed to start: deliver error and close channel. */
        goc_io_read_t* res = (goc_io_read_t*)goc_malloc(sizeof(goc_io_read_t));
        res->nread = rc;
        res->buf   = NULL;
        goc_put_cb(d->ch, res, NULL, NULL);
        goc_close(d->ch);
        d->stream->data = NULL;
    }

    uv_close((uv_handle_t*)h, unregister_io_handle);
}

goc_chan* goc_io_read_start(uv_stream_t* stream)
{
    goc_chan*                   ch = goc_chan_make(16);
    goc_read_start_dispatch_t*  d  = (goc_read_start_dispatch_t*)goc_malloc(
                                         sizeof(goc_read_start_dispatch_t));
    d->stream = stream;
    d->ch     = ch;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_read_start_dispatch, "goc_io_read_start");
    return ch;
}

/* -------------------------------------------------------------------------
 * goc_io_read_stop
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_async_t   async;   /* MUST be first member */
    uv_stream_t* stream;
} goc_read_stop_dispatch_t;

static void on_read_stop_dispatch(uv_async_t* h)
{
    goc_read_stop_dispatch_t* d = (goc_read_stop_dispatch_t*)h;

    uv_read_stop(d->stream);

    if (d->stream->data) {
        goc_stream_ctx_t* ctx = (goc_stream_ctx_t*)d->stream->data;
        goc_close(ctx->ch);
        d->stream->data = NULL;
    }

    uv_close((uv_handle_t*)h, unregister_io_handle);
}

int goc_io_read_stop(uv_stream_t* stream)
{
    goc_read_stop_dispatch_t* d = (goc_read_stop_dispatch_t*)goc_malloc(
                                      sizeof(goc_read_stop_dispatch_t));
    d->stream = stream;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_read_stop_dispatch, "goc_io_read_stop");
    return 0;
}

/* -------------------------------------------------------------------------
 * goc_io_write
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_write_t  req;    /* MUST be first member */
    goc_chan*   ch;
} goc_write_ctx_t;

static void on_write_cb(uv_write_t* req, int status)
{
    goc_write_ctx_t* ctx = (goc_write_ctx_t*)req;
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(status), NULL, NULL);
    goc_close(ctx->ch);
}

typedef struct {
    uv_async_t      async;    /* MUST be first member */
    uv_stream_t*    handle;
    const uv_buf_t* bufs;
    unsigned int    nbufs;
    goc_chan*       ch;
} goc_write_dispatch_t;

static void on_write_dispatch(uv_async_t* h)
{
    goc_write_dispatch_t* d   = (goc_write_dispatch_t*)h;
    goc_write_ctx_t*      ctx = (goc_write_ctx_t*)goc_malloc(sizeof(goc_write_ctx_t));
    ctx->ch = d->ch;
    int rc = uv_write(&ctx->req, d->handle, d->bufs, d->nbufs, on_write_cb);
    if (rc < 0) {
        goc_put_cb(ctx->ch, SCALAR(rc), NULL, NULL);
        goc_close(ctx->ch);
    } else {
        gc_handle_register(ctx);
    }
    uv_close((uv_handle_t*)h, unregister_io_handle);
}

goc_chan* goc_io_write(uv_stream_t* handle,
                          const uv_buf_t bufs[], unsigned int nbufs)
{
    goc_chan*             ch = goc_chan_make(1);
    goc_write_dispatch_t* d  = (goc_write_dispatch_t*)goc_malloc(
                                    sizeof(goc_write_dispatch_t));
    d->handle = handle;
    d->bufs   = bufs;
    d->nbufs  = nbufs;
    d->ch     = ch;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_write_dispatch, "goc_io_write");
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_write2  (IPC streams)
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_write_t   req;         /* MUST be first member */
    goc_chan*    ch;
} goc_write2_ctx_t;

/* Reuse on_write_cb for write2 (same signature, same semantics). */
static void on_write2_cb(uv_write_t* req, int status)
{
    goc_write2_ctx_t* ctx = (goc_write2_ctx_t*)req;
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(status), NULL, NULL);
    goc_close(ctx->ch);
}

typedef struct {
    uv_async_t      async;        /* MUST be first member */
    uv_stream_t*    handle;
    const uv_buf_t* bufs;
    unsigned int    nbufs;
    uv_stream_t*    send_handle;
    goc_chan*       ch;
} goc_write2_dispatch_t;

static void on_write2_dispatch(uv_async_t* h)
{
    goc_write2_dispatch_t* d   = (goc_write2_dispatch_t*)h;
    goc_write2_ctx_t*      ctx = (goc_write2_ctx_t*)goc_malloc(
                                     sizeof(goc_write2_ctx_t));
    ctx->ch = d->ch;
    int rc = uv_write2(&ctx->req, d->handle, d->bufs, d->nbufs,
                       d->send_handle, on_write2_cb);
    if (rc < 0) {
        goc_put_cb(ctx->ch, SCALAR(rc), NULL, NULL);
        goc_close(ctx->ch);
    } else {
        gc_handle_register(ctx);
    }
    uv_close((uv_handle_t*)h, unregister_io_handle);
}

goc_chan* goc_io_write2(uv_stream_t* handle,
                           const uv_buf_t bufs[], unsigned int nbufs,
                           uv_stream_t* send_handle)
{
    goc_chan*              ch = goc_chan_make(1);
    goc_write2_dispatch_t* d  = (goc_write2_dispatch_t*)goc_malloc(
                                     sizeof(goc_write2_dispatch_t));
    d->handle      = handle;
    d->bufs        = bufs;
    d->nbufs       = nbufs;
    d->send_handle = send_handle;
    d->ch          = ch;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_write2_dispatch, "goc_io_write2");
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_shutdown_stream
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_shutdown_t req;   /* MUST be first member */
    goc_chan*     ch;
} goc_shutdown_ctx_t;

static void on_shutdown_cb(uv_shutdown_t* req, int status)
{
    goc_shutdown_ctx_t* ctx = (goc_shutdown_ctx_t*)req;
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(status), NULL, NULL);
    goc_close(ctx->ch);
}

typedef struct {
    uv_async_t   async;    /* MUST be first member */
    uv_stream_t* handle;
    goc_chan*    ch;
} goc_shutdown_dispatch_t;

static void on_shutdown_dispatch(uv_async_t* h)
{
    goc_shutdown_dispatch_t* d   = (goc_shutdown_dispatch_t*)h;
    goc_shutdown_ctx_t*      ctx = (goc_shutdown_ctx_t*)goc_malloc(
                                       sizeof(goc_shutdown_ctx_t));
    ctx->ch = d->ch;
    int rc = uv_shutdown(&ctx->req, d->handle, on_shutdown_cb);
    if (rc < 0) {
        goc_put_cb(ctx->ch, SCALAR(rc), NULL, NULL);
        goc_close(ctx->ch);
    } else {
        gc_handle_register(ctx);
    }
    uv_close((uv_handle_t*)h, unregister_io_handle);
}

goc_chan* goc_io_shutdown_stream(uv_stream_t* handle)
{
    goc_chan*                ch = goc_chan_make(1);
    goc_shutdown_dispatch_t* d  = (goc_shutdown_dispatch_t*)goc_malloc(
                                      sizeof(goc_shutdown_dispatch_t));
    d->handle = handle;
    d->ch     = ch;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_shutdown_dispatch, "goc_io_shutdown_stream");
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_tcp_connect
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_connect_t req;   /* MUST be first member */
    goc_chan*    ch;
} goc_connect_ctx_t;

static void on_connect_cb(uv_connect_t* req, int status)
{
    goc_connect_ctx_t* ctx = (goc_connect_ctx_t*)req;
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(status), NULL, NULL);
    goc_close(ctx->ch);
}

typedef struct {
    uv_async_t          async;    /* MUST be first member */
    uv_tcp_t*           handle;
    struct sockaddr_storage addr; /* copy of the target address */
    goc_chan*           ch;
} goc_tcp_connect_dispatch_t;

static void on_tcp_connect_dispatch(uv_async_t* h)
{
    goc_tcp_connect_dispatch_t* d   = (goc_tcp_connect_dispatch_t*)h;
    goc_connect_ctx_t*          ctx = (goc_connect_ctx_t*)goc_malloc(
                                          sizeof(goc_connect_ctx_t));
    ctx->ch = d->ch;
    int rc = uv_tcp_connect(&ctx->req, d->handle,
                            (const struct sockaddr*)&d->addr,
                            on_connect_cb);
    if (rc < 0) {
        goc_put_cb(ctx->ch, SCALAR(rc), NULL, NULL);
        goc_close(ctx->ch);
    } else {
        gc_handle_register(ctx);
    }
    uv_close((uv_handle_t*)h, unregister_io_handle);
}

goc_chan* goc_io_tcp_connect(uv_tcp_t* handle, const struct sockaddr* addr)
{
    goc_chan*                   ch = goc_chan_make(1);
    goc_tcp_connect_dispatch_t* d  = (goc_tcp_connect_dispatch_t*)goc_malloc(
                                         sizeof(goc_tcp_connect_dispatch_t));
    d->handle = handle;
    /* Copy the address to avoid dangling-pointer issues if the caller's
     * address lives on a stack frame that may be reused before the async
     * dispatch fires on the loop thread. */
    memcpy(&d->addr, addr,
           addr->sa_family == AF_INET6
               ? sizeof(struct sockaddr_in6)
               : sizeof(struct sockaddr_in));
    d->ch = ch;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_tcp_connect_dispatch, "goc_io_tcp_connect");
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_pipe_connect
 * ---------------------------------------------------------------------- */

/* uv_pipe_connect has no return code; the callback always fires. */

typedef struct {
    uv_async_t   async;    /* MUST be first member */
    uv_pipe_t*   handle;
    char*        name;     /* malloc-copied pipe name */
    goc_chan*    ch;
} goc_pipe_connect_dispatch_t;

static void on_pipe_connect_dispatch(uv_async_t* h)
{
    goc_pipe_connect_dispatch_t* d   = (goc_pipe_connect_dispatch_t*)h;
    goc_connect_ctx_t*           ctx = (goc_connect_ctx_t*)goc_malloc(
                                           sizeof(goc_connect_ctx_t));
    ctx->ch = d->ch;
    uv_pipe_connect(&ctx->req, d->handle, d->name, on_connect_cb);
    gc_handle_register(ctx);
    uv_close((uv_handle_t*)h, unregister_io_handle);
}

goc_chan* goc_io_pipe_connect(uv_pipe_t* handle, const char* name)
{
    goc_chan*                    ch = goc_chan_make(1);
    goc_pipe_connect_dispatch_t* d  = (goc_pipe_connect_dispatch_t*)goc_malloc(
                                          sizeof(goc_pipe_connect_dispatch_t));
    d->handle = handle;
    size_t name_len = strlen(name);
    d->name   = (char*)goc_malloc(name_len + 1);   /* copied so the caller's string can be freed */
    strcpy(d->name, name);
    d->ch = ch;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_pipe_connect_dispatch, "goc_io_pipe_connect");
    return ch;
}


/* =========================================================================
 * 2. UDP (Datagrams)
 * ====================================================================== */

/* -------------------------------------------------------------------------
 * goc_io_udp_send
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_udp_send_t req;   /* MUST be first member */
    goc_chan*     ch;
} goc_udp_send_ctx_t;

static void on_udp_send_cb(uv_udp_send_t* req, int status)
{
    goc_udp_send_ctx_t* ctx = (goc_udp_send_ctx_t*)req;
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(status), NULL, NULL);
    goc_close(ctx->ch);
}

typedef struct {
    uv_async_t              async;    /* MUST be first member */
    uv_udp_t*               handle;
    const uv_buf_t*         bufs;
    unsigned int            nbufs;
    struct sockaddr_storage addr;     /* copy of destination address */
    goc_chan*               ch;
} goc_udp_send_dispatch_t;

static void on_udp_send_dispatch(uv_async_t* h)
{
    goc_udp_send_dispatch_t* d   = (goc_udp_send_dispatch_t*)h;
    goc_udp_send_ctx_t*      ctx = (goc_udp_send_ctx_t*)goc_malloc(
                                       sizeof(goc_udp_send_ctx_t));
    ctx->ch = d->ch;
    int rc = uv_udp_send(&ctx->req, d->handle, d->bufs, d->nbufs,
                         (const struct sockaddr*)&d->addr, on_udp_send_cb);
    if (rc < 0) {
        goc_put_cb(ctx->ch, SCALAR(rc), NULL, NULL);
        goc_close(ctx->ch);
    } else {
        gc_handle_register(ctx);
    }
    uv_close((uv_handle_t*)h, unregister_io_handle);
}

goc_chan* goc_io_udp_send(uv_udp_t* handle,
                             const uv_buf_t bufs[], unsigned int nbufs,
                             const struct sockaddr* addr)
{
    goc_chan*                ch = goc_chan_make(1);
    goc_udp_send_dispatch_t* d  = (goc_udp_send_dispatch_t*)goc_malloc(
                                      sizeof(goc_udp_send_dispatch_t));
    d->handle = handle;
    d->bufs   = bufs;
    d->nbufs  = nbufs;
    memcpy(&d->addr, addr,
           addr->sa_family == AF_INET6
               ? sizeof(struct sockaddr_in6)
               : sizeof(struct sockaddr_in));
    d->ch = ch;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_udp_send_dispatch, "goc_io_udp_send");
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_udp_recv_start / goc_io_udp_recv_stop
 * ---------------------------------------------------------------------- */

typedef struct {
    goc_chan* ch;
} goc_udp_recv_ctx_t;

static void on_udp_recv_cb(uv_udp_t* handle, ssize_t nread,
                           const uv_buf_t* buf, const struct sockaddr* addr,
                           unsigned flags)
{
    goc_udp_recv_ctx_t* ctx = (goc_udp_recv_ctx_t*)handle->data;

    if (nread == 0 && addr == NULL) {
        /* libuv fires this when there is no more data; free unused buffer. */
        free(buf->base);
        return;
    }

    goc_io_udp_recv_t* res = (goc_io_udp_recv_t*)goc_malloc(sizeof(goc_io_udp_recv_t));
    res->nread = nread;

    if (nread > 0) {
        /* Normal datagram: copy into GC-managed buffer and addr. */
        uv_buf_t* gc_buf = (uv_buf_t*)goc_malloc(sizeof(uv_buf_t));
        gc_buf->base = (char*)goc_malloc((size_t)nread);
        gc_buf->len  = (size_t)nread;
        memcpy(gc_buf->base, buf->base, (size_t)nread);
        free(buf->base);
        res->buf = gc_buf;

        if (addr) {
            size_t addr_size = (addr->sa_family == AF_INET6)
                             ? sizeof(struct sockaddr_in6)
                             : sizeof(struct sockaddr_in);
            struct sockaddr* gc_addr = (struct sockaddr*)goc_malloc(addr_size);
            memcpy(gc_addr, addr, addr_size);
            res->addr = gc_addr;
        } else {
            res->addr = NULL;
        }
        res->flags = flags;
        goc_put_cb(ctx->ch, res, NULL, NULL);
        return;
    }

    /* nread < 0: Error. */
    free(buf->base);
    res->buf  = NULL;
    res->addr = NULL;
    res->flags = 0;
    goc_put_cb(ctx->ch, res, NULL, NULL);
    goc_close(ctx->ch);
    handle->data = NULL;
}

typedef struct {
    uv_async_t  async;    /* MUST be first member */
    uv_udp_t*   handle;
    goc_chan*   ch;
} goc_udp_recv_start_dispatch_t;

static void on_udp_recv_start_dispatch(uv_async_t* h)
{
    goc_udp_recv_start_dispatch_t* d = (goc_udp_recv_start_dispatch_t*)h;

    goc_udp_recv_ctx_t* ctx = (goc_udp_recv_ctx_t*)goc_malloc(
                                  sizeof(goc_udp_recv_ctx_t));
    ctx->ch        = d->ch;
    d->handle->data = ctx;

    int rc = uv_udp_recv_start(d->handle, goc_alloc_cb, on_udp_recv_cb);
    if (rc < 0) {
        goc_io_udp_recv_t* res = (goc_io_udp_recv_t*)goc_malloc(sizeof(goc_io_udp_recv_t));
        res->nread = rc;
        res->buf   = NULL;
        res->addr  = NULL;
        res->flags = 0;
        goc_put_cb(d->ch, res, NULL, NULL);
        goc_close(d->ch);
        d->handle->data = NULL;
    }

    uv_close((uv_handle_t*)h, unregister_io_handle);
}

goc_chan* goc_io_udp_recv_start(uv_udp_t* handle)
{
    goc_chan*                      ch = goc_chan_make(16);
    goc_udp_recv_start_dispatch_t* d  = (goc_udp_recv_start_dispatch_t*)goc_malloc(
                                            sizeof(goc_udp_recv_start_dispatch_t));
    d->handle = handle;
    d->ch     = ch;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_udp_recv_start_dispatch, "goc_io_udp_recv_start");
    return ch;
}

typedef struct {
    uv_async_t  async;    /* MUST be first member */
    uv_udp_t*   handle;
} goc_udp_recv_stop_dispatch_t;

static void on_udp_recv_stop_dispatch(uv_async_t* h)
{
    goc_udp_recv_stop_dispatch_t* d = (goc_udp_recv_stop_dispatch_t*)h;

    uv_udp_recv_stop(d->handle);

    if (d->handle->data) {
        goc_udp_recv_ctx_t* ctx = (goc_udp_recv_ctx_t*)d->handle->data;
        goc_close(ctx->ch);
        d->handle->data = NULL;
    }

    uv_close((uv_handle_t*)h, unregister_io_handle);
}

int goc_io_udp_recv_stop(uv_udp_t* handle)
{
    goc_udp_recv_stop_dispatch_t* d = (goc_udp_recv_stop_dispatch_t*)goc_malloc(
                                          sizeof(goc_udp_recv_stop_dispatch_t));
    d->handle = handle;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_udp_recv_stop_dispatch, "goc_io_udp_recv_stop");
    return 0;
}

/* =========================================================================
 * 5. GC handle lifetime management
 * ====================================================================== */

void goc_io_handle_register(uv_handle_t* handle)
{
    gc_handle_register(handle);
}

void goc_io_handle_unregister(uv_handle_t* handle)
{
    gc_handle_unregister(handle);
}

typedef struct {
    uv_close_cb user_cb;
} goc_handle_close_ctx_t;

static void on_goc_handle_close(uv_handle_t* handle)
{
    goc_handle_close_ctx_t* ctx = (goc_handle_close_ctx_t*)handle->data;
    uv_close_cb user_cb = ctx ? ctx->user_cb : NULL;
    handle->data = NULL;
    gc_handle_unregister(ctx);
    gc_handle_unregister(handle);
    if (user_cb)
        user_cb(handle);
}

typedef struct {
    uv_async_t   async;    /* MUST be first member */
    uv_handle_t* target;
    uv_close_cb  user_cb;
} goc_handle_close_dispatch_t;

static void on_handle_close_dispatch(uv_async_t* h)
{
    goc_handle_close_dispatch_t* d = (goc_handle_close_dispatch_t*)h;
    goc_handle_close_ctx_t* ctx = (goc_handle_close_ctx_t*)goc_malloc(
                                      sizeof(goc_handle_close_ctx_t));
    ctx->user_cb    = d->user_cb;
    gc_handle_register(ctx);
    d->target->data = ctx;
    uv_close(d->target, on_goc_handle_close);
    uv_close((uv_handle_t*)h, unregister_io_handle);
}

void goc_io_handle_close(uv_handle_t* handle, uv_close_cb cb)
{
    goc_handle_close_dispatch_t* d = (goc_handle_close_dispatch_t*)goc_malloc(
                                         sizeof(goc_handle_close_dispatch_t));
    d->target  = handle;
    d->user_cb = cb;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_handle_close_dispatch, "goc_io_handle_close");
}
