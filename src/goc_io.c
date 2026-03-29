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
#include "../include/goc_array.h"
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

typedef struct {
    uv_fs_t   req;   /* MUST be first member */
    goc_chan*  ch;
    char*      raw;   /* goc_malloc'd buffer for the actual read */
    size_t     len;
} goc_fs_read_ctx_t;

static void on_fs_read(uv_fs_t* req)
{
    goc_fs_read_ctx_t* ctx = (goc_fs_read_ctx_t*)req;
    ssize_t nread = (ssize_t)req->result;
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);

    goc_io_fs_read_t* res = (goc_io_fs_read_t*)goc_malloc(sizeof(goc_io_fs_read_t));
    res->nread = nread;
    res->buf   = (nread > 0) ? ctx->raw : NULL;

    goc_put_cb(ctx->ch, res, close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_read(uv_file file, size_t len, int64_t offset)
{
    goc_chan*          ch  = goc_chan_make(1);
    goc_fs_read_ctx_t* ctx = (goc_fs_read_ctx_t*)goc_malloc(sizeof(goc_fs_read_ctx_t));
    ctx->ch  = ch;
    ctx->len = len;
    ctx->raw = (char*)goc_malloc(len + 1);
    ctx->raw[len] = '\0';

    uv_buf_t uvbuf = uv_buf_init(ctx->raw, (unsigned int)len);
    int rc = uv_fs_read(g_loop, &ctx->req, file, &uvbuf, 1, offset, on_fs_read);
    if (rc < 0) {
        goc_io_fs_read_t* res = (goc_io_fs_read_t*)goc_malloc(sizeof(goc_io_fs_read_t));
        res->nread = rc;
        res->buf   = NULL;
        goc_put_cb(ch, res, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_fs_write
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_fs_t   req;   /* MUST be first member */
    goc_chan* ch;
    char*     raw;   /* goc_malloc'd copy of the data bytes */
} goc_fs_write_ctx_t;

static void on_fs_write(uv_fs_t* req)
{
    goc_fs_write_ctx_t* ctx = (goc_fs_write_ctx_t*)req;
    ssize_t result = (ssize_t)req->result;
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(result), close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_write(uv_file file, goc_array* data, int64_t offset)
{
    goc_chan*            ch  = goc_chan_make(1);
    size_t               n   = goc_array_len(data);
    goc_fs_write_ctx_t*  ctx = (goc_fs_write_ctx_t*)goc_malloc(sizeof(goc_fs_write_ctx_t));
    ctx->ch  = ch;
    ctx->raw = (char*)goc_malloc(n + 1);

    /* Unbox each byte from the goc_array. */
    size_t i;
    for (i = 0; i < n; i++)
        ctx->raw[i] = (char)(unsigned char)goc_unbox_int(goc_array_get(data, i));

    uv_buf_t uvbuf = uv_buf_init(ctx->raw, (unsigned int)n);
    int rc = uv_fs_write(g_loop, &ctx->req, file, &uvbuf, 1, offset, on_fs_write);
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

/* -------------------------------------------------------------------------
 * goc_io_tcp_init / goc_io_pipe_init / goc_io_udp_init /
 * goc_io_tty_init / goc_io_signal_init /
 * goc_io_fs_event_init / goc_io_fs_poll_init
 *
 * Dispatch uv_*_init to the loop thread (since these functions modify loop
 * internals without a lock and must not be called from worker threads).
 * On success the handle is registered as a GC root.
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_async_t  async;   /* MUST be first member */
    void*       handle;
    int         ipc;     /* pipe only */
    uv_file     fd;      /* tty only */
    goc_chan*   ch;
    int         kind;    /* 0=tcp 1=pipe 2=udp 3=tty 4=signal 5=fs_event 6=fs_poll */
} goc_handle_init_dispatch_t;

static void on_handle_init_dispatch(uv_async_t* h)
{
    goc_handle_init_dispatch_t* d = (goc_handle_init_dispatch_t*)h;
    int rc;
    switch (d->kind) {
        case 0: rc = uv_tcp_init(g_loop,      (uv_tcp_t*)d->handle);            break;
        case 1: rc = uv_pipe_init(g_loop,     (uv_pipe_t*)d->handle, d->ipc);   break;
        case 2: rc = uv_udp_init(g_loop,      (uv_udp_t*)d->handle);            break;
        case 3: rc = uv_tty_init(g_loop,      (uv_tty_t*)d->handle, d->fd, 0); break;
        case 4: rc = uv_signal_init(g_loop,   (uv_signal_t*)d->handle);         break;
        case 5: rc = uv_fs_event_init(g_loop, (uv_fs_event_t*)d->handle);       break;
        case 6: rc = uv_fs_poll_init(g_loop,  (uv_fs_poll_t*)d->handle);        break;
        default: rc = UV_EINVAL; break;
    }
    if (rc == 0)
        gc_handle_register(d->handle);
    goc_put_cb(d->ch, SCALAR(rc), close_on_put, d->ch);
    uv_close((uv_handle_t*)h, unregister_io_handle);
}

static goc_chan* handle_init_dispatch(void* handle, int kind, int ipc, uv_file fd)
{
    goc_chan*                    ch = goc_chan_make(1);
    goc_handle_init_dispatch_t*  d  = (goc_handle_init_dispatch_t*)goc_malloc(
                                          sizeof(goc_handle_init_dispatch_t));
    d->handle = handle;
    d->kind   = kind;
    d->ipc    = ipc;
    d->fd     = fd;
    d->ch     = ch;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_handle_init_dispatch, "goc_io_*_init");
    return ch;
}

goc_chan* goc_io_tcp_init(uv_tcp_t* handle)
{
    return handle_init_dispatch(handle, 0, 0, 0);
}

goc_chan* goc_io_pipe_init(uv_pipe_t* handle, int ipc)
{
    return handle_init_dispatch(handle, 1, ipc, 0);
}

goc_chan* goc_io_udp_init(uv_udp_t* handle)
{
    return handle_init_dispatch(handle, 2, 0, 0);
}

goc_chan* goc_io_tty_init(uv_tty_t* handle, uv_file fd)
{
    return handle_init_dispatch(handle, 3, 0, fd);
}

goc_chan* goc_io_signal_init(uv_signal_t* handle)
{
    return handle_init_dispatch(handle, 4, 0, 0);
}

goc_chan* goc_io_fs_event_init(uv_fs_event_t* handle)
{
    return handle_init_dispatch(handle, 5, 0, 0);
}

goc_chan* goc_io_fs_poll_init(uv_fs_poll_t* handle)
{
    return handle_init_dispatch(handle, 6, 0, 0);
}

/* -------------------------------------------------------------------------
 * goc_io_process_spawn
 *
 * uv_spawn both initialises the uv_process_t and starts the child process.
 * The options pointer must remain valid until the channel delivers its value.
 * ---------------------------------------------------------------------- */

/* Context stored in handle->data for process exit tracking. */
typedef struct {
    goc_chan* exit_ch;
} goc_process_ctx_t;

static void on_process_exit(uv_process_t* handle, int64_t exit_status,
                             int term_signal)
{
    goc_process_ctx_t* ctx = (goc_process_ctx_t*)handle->data;
    if (ctx && ctx->exit_ch) {
        goc_io_process_exit_t* res = (goc_io_process_exit_t*)goc_malloc(
                                         sizeof(goc_io_process_exit_t));
        res->exit_status = exit_status;
        res->term_signal = term_signal;
        goc_put_cb(ctx->exit_ch, res, close_on_put, ctx->exit_ch);
    }
}

typedef struct {
    uv_async_t               async;   /* MUST be first member */
    uv_process_t*            handle;
    uv_process_options_t     opts_copy; /* mutable copy */
    goc_chan*                ch;
    goc_chan*                exit_ch;  /* NULL if caller passed NULL */
} goc_process_spawn_dispatch_t;

static void on_process_spawn_dispatch(uv_async_t* h)
{
    goc_process_spawn_dispatch_t* d = (goc_process_spawn_dispatch_t*)h;

    if (d->exit_ch) {
        /* Set up process ctx and exit callback. */
        goc_process_ctx_t* ctx = (goc_process_ctx_t*)goc_malloc(
                                     sizeof(goc_process_ctx_t));
        ctx->exit_ch          = d->exit_ch;
        d->handle->data       = ctx;
        d->opts_copy.exit_cb  = on_process_exit;
    }

    int rc = uv_spawn(g_loop, d->handle, &d->opts_copy);
    if (rc == 0)
        gc_handle_register(d->handle);
    goc_put_cb(d->ch, SCALAR(rc), close_on_put, d->ch);
    uv_close((uv_handle_t*)h, unregister_io_handle);
}

goc_chan* goc_io_process_spawn(uv_process_t* handle,
                               const uv_process_options_t* opts,
                               goc_chan** exit_ch)
{
    goc_chan*                      ch       = goc_chan_make(1);
    goc_chan*                      ech      = NULL;
    goc_process_spawn_dispatch_t*  d        = (goc_process_spawn_dispatch_t*)goc_malloc(
                                                  sizeof(goc_process_spawn_dispatch_t));
    if (exit_ch) {
        ech      = goc_chan_make(1);
        *exit_ch = ech;
    }
    d->handle    = handle;
    d->opts_copy = *opts;  /* shallow copy */
    d->ch        = ch;
    d->exit_ch   = ech;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_process_spawn_dispatch, "goc_io_process_spawn");
    return ch;
}

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

/* =========================================================================
 * TCP bind / server / socket options
 * ====================================================================== */

/* Generic dispatch: one handle pointer + one int result (SCALAR). */
typedef struct {
    uv_async_t  async;   /* MUST be first member */
    void*       handle;
    goc_chan*   ch;
    int         kind;
    /* payload fields (used depending on kind) */
    struct sockaddr_storage addr;
    int         i1;
    unsigned    u1;
    unsigned    u2;
    char        s1[256];
    char        s2[256];
    char        s3[256];
    uv_membership membership;
} goc_simple_dispatch_t;

static void on_simple_dispatch(uv_async_t* h)
{
    goc_simple_dispatch_t* d = (goc_simple_dispatch_t*)h;
    int rc = 0;
    switch (d->kind) {
        /* TCP */
        case 0:  rc = uv_tcp_bind((uv_tcp_t*)d->handle,
                                  (const struct sockaddr*)&d->addr, 0); break;
        case 1:  rc = uv_tcp_keepalive((uv_tcp_t*)d->handle, d->i1, d->u1); break;
        case 2:  rc = uv_tcp_nodelay((uv_tcp_t*)d->handle, d->i1); break;
        case 3:  rc = uv_tcp_simultaneous_accepts((uv_tcp_t*)d->handle, d->i1); break;
        /* UDP */
        case 10: rc = uv_udp_bind((uv_udp_t*)d->handle,
                                  (const struct sockaddr*)&d->addr, d->u1); break;
        case 11: rc = uv_udp_connect((uv_udp_t*)d->handle,
                                     (const struct sockaddr*)&d->addr); break;
        case 12: rc = uv_udp_set_broadcast((uv_udp_t*)d->handle, d->i1); break;
        case 13: rc = uv_udp_set_ttl((uv_udp_t*)d->handle, d->i1); break;
        case 14: rc = uv_udp_set_multicast_ttl((uv_udp_t*)d->handle, d->i1); break;
        case 15: rc = uv_udp_set_multicast_loop((uv_udp_t*)d->handle, d->i1); break;
        case 16: rc = uv_udp_set_multicast_interface((uv_udp_t*)d->handle, d->s1); break;
        case 17: rc = uv_udp_set_membership((uv_udp_t*)d->handle,
                                            d->s1, d->s2[0] ? d->s2 : NULL,
                                            d->membership); break;
        case 18: rc = uv_udp_set_source_membership((uv_udp_t*)d->handle,
                                                   d->s1,
                                                   d->s2[0] ? d->s2 : NULL,
                                                   d->s3, d->membership); break;
        /* TTY */
        case 20: rc = uv_tty_set_mode((uv_tty_t*)d->handle,
                                      (uv_tty_mode_t)d->i1); break;
        /* process kill */
        case 30: rc = uv_process_kill((uv_process_t*)d->handle, d->i1); break;
        case 31: rc = uv_kill(d->i1, (int)d->u1); break;
        /* pipe bind */
        case 40: rc = uv_pipe_bind((uv_pipe_t*)d->handle, d->s1); break;
        default: rc = UV_EINVAL; break;
    }
    goc_put_cb(d->ch, SCALAR(rc), close_on_put, d->ch);
    uv_close((uv_handle_t*)h, unregister_io_handle);
}

static goc_chan* simple_dispatch(void* handle, int kind,
                                 const struct sockaddr* addr,
                                 int i1, unsigned u1,
                                 const char* s1, const char* s2,
                                 const char* s3,
                                 uv_membership membership)
{
    goc_chan*               ch = goc_chan_make(1);
    goc_simple_dispatch_t*  d  = (goc_simple_dispatch_t*)goc_malloc(
                                     sizeof(goc_simple_dispatch_t));
    d->handle     = handle;
    d->kind       = kind;
    d->ch         = ch;
    d->i1         = i1;
    d->u1         = u1;
    d->membership = membership;
    if (addr)
        memcpy(&d->addr, addr,
               addr->sa_family == AF_INET6
                   ? sizeof(struct sockaddr_in6)
                   : sizeof(struct sockaddr_in));
    if (s1) { strncpy(d->s1, s1, sizeof(d->s1) - 1); d->s1[sizeof(d->s1)-1] = '\0'; }
    else d->s1[0] = '\0';
    if (s2) { strncpy(d->s2, s2, sizeof(d->s2) - 1); d->s2[sizeof(d->s2)-1] = '\0'; }
    else d->s2[0] = '\0';
    if (s3) { strncpy(d->s3, s3, sizeof(d->s3) - 1); d->s3[sizeof(d->s3)-1] = '\0'; }
    else d->s3[0] = '\0';
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_simple_dispatch, "goc_io_simple");
    return ch;
}

goc_chan* goc_io_tcp_bind(uv_tcp_t* handle, const struct sockaddr* addr)
{
    return simple_dispatch(handle, 0, addr, 0, 0, NULL, NULL, NULL, 0);
}

goc_chan* goc_io_tcp_keepalive(uv_tcp_t* handle, int enable, unsigned int delay)
{
    return simple_dispatch(handle, 1, NULL, enable, delay, NULL, NULL, NULL, 0);
}

goc_chan* goc_io_tcp_nodelay(uv_tcp_t* handle, int enable)
{
    return simple_dispatch(handle, 2, NULL, enable, 0, NULL, NULL, NULL, 0);
}

goc_chan* goc_io_tcp_simultaneous_accepts(uv_tcp_t* handle, int enable)
{
    return simple_dispatch(handle, 3, NULL, enable, 0, NULL, NULL, NULL, 0);
}

goc_chan* goc_io_pipe_bind(uv_pipe_t* handle, const char* name)
{
    return simple_dispatch(handle, 40, NULL, 0, 0, name, NULL, NULL, 0);
}

goc_chan* goc_io_udp_bind(uv_udp_t* handle, const struct sockaddr* addr,
                          unsigned flags)
{
    return simple_dispatch(handle, 10, addr, 0, flags, NULL, NULL, NULL, 0);
}

goc_chan* goc_io_udp_connect(uv_udp_t* handle, const struct sockaddr* addr)
{
    return simple_dispatch(handle, 11, addr, 0, 0, NULL, NULL, NULL, 0);
}

goc_chan* goc_io_udp_set_broadcast(uv_udp_t* handle, int on)
{
    return simple_dispatch(handle, 12, NULL, on, 0, NULL, NULL, NULL, 0);
}

goc_chan* goc_io_udp_set_ttl(uv_udp_t* handle, int ttl)
{
    return simple_dispatch(handle, 13, NULL, ttl, 0, NULL, NULL, NULL, 0);
}

goc_chan* goc_io_udp_set_multicast_ttl(uv_udp_t* handle, int ttl)
{
    return simple_dispatch(handle, 14, NULL, ttl, 0, NULL, NULL, NULL, 0);
}

goc_chan* goc_io_udp_set_multicast_loop(uv_udp_t* handle, int on)
{
    return simple_dispatch(handle, 15, NULL, on, 0, NULL, NULL, NULL, 0);
}

goc_chan* goc_io_udp_set_multicast_interface(uv_udp_t* handle,
                                             const char* iface_addr)
{
    return simple_dispatch(handle, 16, NULL, 0, 0, iface_addr, NULL, NULL, 0);
}

goc_chan* goc_io_udp_set_membership(uv_udp_t* handle,
                                    const char* mcast_addr,
                                    const char* iface_addr,
                                    uv_membership membership)
{
    return simple_dispatch(handle, 17, NULL, 0, 0, mcast_addr, iface_addr,
                           NULL, membership);
}

goc_chan* goc_io_udp_set_source_membership(uv_udp_t* handle,
                                           const char* mcast_addr,
                                           const char* iface_addr,
                                           const char* source_addr,
                                           uv_membership membership)
{
    return simple_dispatch(handle, 18, NULL, 0, 0, mcast_addr, iface_addr,
                           source_addr, membership);
}

goc_chan* goc_io_tty_set_mode(uv_tty_t* handle, uv_tty_mode_t mode)
{
    return simple_dispatch(handle, 20, NULL, (int)mode, 0, NULL, NULL, NULL, 0);
}

goc_chan* goc_io_process_kill(uv_process_t* handle, int signum)
{
    return simple_dispatch(handle, 30, NULL, signum, 0, NULL, NULL, NULL, 0);
}

goc_chan* goc_io_kill(int pid, int signum)
{
    /* Reuse simple_dispatch with kind=31; i1=pid, u1=signum */
    goc_chan*               ch = goc_chan_make(1);
    goc_simple_dispatch_t*  d  = (goc_simple_dispatch_t*)goc_malloc(
                                     sizeof(goc_simple_dispatch_t));
    d->handle = NULL;
    d->kind   = 31;
    d->ch     = ch;
    d->i1     = pid;
    d->u1     = (unsigned)signum;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_simple_dispatch, "goc_io_kill");
    return ch;
}

/* =========================================================================
 * TCP server / Pipe server
 * ====================================================================== */

typedef struct {
    goc_chan* ch;
    int       is_tcp; /* 1=tcp 0=pipe */
} goc_server_ctx_t;

static void on_server_connection(uv_stream_t* server, int status)
{
    goc_server_ctx_t* ctx = (goc_server_ctx_t*)server->data;
    if (status < 0) {
        /* Error: close channel. */
        goc_close(ctx->ch);
        server->data = NULL;
        return;
    }

    if (ctx->is_tcp) {
        uv_tcp_t* client = (uv_tcp_t*)goc_malloc(sizeof(uv_tcp_t));
        int rc = uv_tcp_init(g_loop, client);
        if (rc < 0) {
            goc_close(ctx->ch);
            server->data = NULL;
            return;
        }
        gc_handle_register(client);
        rc = uv_accept(server, (uv_stream_t*)client);
        if (rc < 0) {
            gc_handle_unregister(client);
            uv_close((uv_handle_t*)client, NULL);
            return;
        }
        goc_put_cb(ctx->ch, client, NULL, NULL);
    } else {
        uv_pipe_t* client = (uv_pipe_t*)goc_malloc(sizeof(uv_pipe_t));
        int rc = uv_pipe_init(g_loop, client, 0);
        if (rc < 0) {
            goc_close(ctx->ch);
            server->data = NULL;
            return;
        }
        gc_handle_register(client);
        rc = uv_accept(server, (uv_stream_t*)client);
        if (rc < 0) {
            gc_handle_unregister(client);
            uv_close((uv_handle_t*)client, NULL);
            return;
        }
        goc_put_cb(ctx->ch, client, NULL, NULL);
    }
}

typedef struct {
    uv_async_t    async;   /* MUST be first member */
    uv_stream_t*  server;
    int           backlog;
    goc_chan*     ch;
    int           is_tcp;
} goc_server_make_dispatch_t;

static void on_server_make_dispatch(uv_async_t* h)
{
    goc_server_make_dispatch_t* d = (goc_server_make_dispatch_t*)h;

    goc_server_ctx_t* ctx = (goc_server_ctx_t*)goc_malloc(sizeof(goc_server_ctx_t));
    ctx->ch      = d->ch;
    ctx->is_tcp  = d->is_tcp;
    d->server->data = ctx;

    int rc = uv_listen(d->server, d->backlog, on_server_connection);
    if (rc < 0) {
        /* Deliver error and close channel. */
        goc_close(d->ch);
        d->server->data = NULL;
    }

    uv_close((uv_handle_t*)h, unregister_io_handle);
}

goc_chan* goc_io_tcp_server_make(uv_tcp_t* handle, int backlog)
{
    goc_chan*                    ch = goc_chan_make(16);
    goc_server_make_dispatch_t*  d  = (goc_server_make_dispatch_t*)goc_malloc(
                                          sizeof(goc_server_make_dispatch_t));
    d->server  = (uv_stream_t*)handle;
    d->backlog = backlog;
    d->ch      = ch;
    d->is_tcp  = 1;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_server_make_dispatch, "goc_io_tcp_server_make");
    return ch;
}

goc_chan* goc_io_pipe_server_make(uv_pipe_t* handle, int backlog)
{
    goc_chan*                    ch = goc_chan_make(16);
    goc_server_make_dispatch_t*  d  = (goc_server_make_dispatch_t*)goc_malloc(
                                          sizeof(goc_server_make_dispatch_t));
    d->server  = (uv_stream_t*)handle;
    d->backlog = backlog;
    d->ch      = ch;
    d->is_tcp  = 0;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_server_make_dispatch, "goc_io_pipe_server_make");
    return ch;
}

/* =========================================================================
 * TTY get winsize
 * ====================================================================== */

typedef struct {
    uv_async_t  async;   /* MUST be first member */
    uv_tty_t*   handle;
    goc_chan*   ch;
} goc_tty_winsize_dispatch_t;

static void on_tty_winsize_dispatch(uv_async_t* h)
{
    goc_tty_winsize_dispatch_t* d = (goc_tty_winsize_dispatch_t*)h;
    goc_io_tty_winsize_t* res = (goc_io_tty_winsize_t*)goc_malloc(
                                    sizeof(goc_io_tty_winsize_t));
    int w = 0, ht = 0;
    int rc = uv_tty_get_winsize(d->handle, &w, &ht);
    res->ok     = (rc == 0) ? GOC_IO_OK : GOC_IO_ERR;
    res->width  = w;
    res->height = ht;
    goc_put_cb(d->ch, res, close_on_put, d->ch);
    uv_close((uv_handle_t*)h, unregister_io_handle);
}

goc_chan* goc_io_tty_get_winsize(uv_tty_t* handle)
{
    goc_chan*                    ch = goc_chan_make(1);
    goc_tty_winsize_dispatch_t*  d  = (goc_tty_winsize_dispatch_t*)goc_malloc(
                                          sizeof(goc_tty_winsize_dispatch_t));
    d->handle = handle;
    d->ch     = ch;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_tty_winsize_dispatch, "goc_io_tty_get_winsize");
    return ch;
}

/* =========================================================================
 * Signal start / stop
 * ====================================================================== */

typedef struct {
    goc_chan* ch;
    int       signum;
} goc_signal_ctx_t;

static void on_signal_cb(uv_signal_t* handle, int signum)
{
    goc_signal_ctx_t* ctx = (goc_signal_ctx_t*)handle->data;
    if (ctx && ctx->ch)
        goc_put_cb(ctx->ch, SCALAR(signum), NULL, NULL);
}

typedef struct {
    uv_async_t    async;   /* MUST be first member */
    uv_signal_t*  handle;
    int           signum;
    goc_chan*     ch;
} goc_signal_start_dispatch_t;

static void on_signal_start_dispatch(uv_async_t* h)
{
    goc_signal_start_dispatch_t* d = (goc_signal_start_dispatch_t*)h;

    goc_signal_ctx_t* ctx = (goc_signal_ctx_t*)goc_malloc(sizeof(goc_signal_ctx_t));
    ctx->ch     = d->ch;
    ctx->signum = d->signum;
    d->handle->data = ctx;

    int rc = uv_signal_start(d->handle, on_signal_cb, d->signum);
    if (rc < 0) {
        goc_close(d->ch);
        d->handle->data = NULL;
    }

    uv_close((uv_handle_t*)h, unregister_io_handle);
}

goc_chan* goc_io_signal_start(uv_signal_t* handle, int signum)
{
    goc_chan*                    ch = goc_chan_make(16);
    goc_signal_start_dispatch_t* d  = (goc_signal_start_dispatch_t*)goc_malloc(
                                          sizeof(goc_signal_start_dispatch_t));
    d->handle = handle;
    d->signum = signum;
    d->ch     = ch;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_signal_start_dispatch, "goc_io_signal_start");
    return ch;
}

typedef struct {
    uv_async_t   async;   /* MUST be first member */
    uv_signal_t* handle;
} goc_signal_stop_dispatch_t;

static void on_signal_stop_dispatch(uv_async_t* h)
{
    goc_signal_stop_dispatch_t* d = (goc_signal_stop_dispatch_t*)h;
    uv_signal_stop(d->handle);
    if (d->handle->data) {
        goc_signal_ctx_t* ctx = (goc_signal_ctx_t*)d->handle->data;
        goc_close(ctx->ch);
        d->handle->data = NULL;
    }
    uv_close((uv_handle_t*)h, unregister_io_handle);
}

int goc_io_signal_stop(uv_signal_t* handle)
{
    goc_signal_stop_dispatch_t* d = (goc_signal_stop_dispatch_t*)goc_malloc(
                                        sizeof(goc_signal_stop_dispatch_t));
    d->handle = handle;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_signal_stop_dispatch, "goc_io_signal_stop");
    return 0;
}

/* =========================================================================
 * FS event start / stop
 * ====================================================================== */

typedef struct {
    goc_chan* ch;
} goc_fs_event_ctx_t;

static void on_fs_event_cb(uv_fs_event_t* handle, const char* filename,
                           int events, int status)
{
    goc_fs_event_ctx_t* ctx = (goc_fs_event_ctx_t*)handle->data;
    if (!ctx || !ctx->ch) return;

    goc_io_fs_event_t* res = (goc_io_fs_event_t*)goc_malloc(sizeof(goc_io_fs_event_t));
    res->ok     = (status == 0) ? GOC_IO_OK : GOC_IO_ERR;
    res->events = events;
    if (filename) {
        size_t len = strlen(filename);
        char*  buf = (char*)goc_malloc(len + 1);
        memcpy(buf, filename, len + 1);
        res->filename = buf;
    } else {
        res->filename = NULL;
    }
    goc_put_cb(ctx->ch, res, NULL, NULL);
}

typedef struct {
    uv_async_t      async;   /* MUST be first member */
    uv_fs_event_t*  handle;
    char*           path;
    unsigned        flags;
    goc_chan*       ch;
} goc_fs_event_start_dispatch_t;

static void on_fs_event_start_dispatch(uv_async_t* h)
{
    goc_fs_event_start_dispatch_t* d = (goc_fs_event_start_dispatch_t*)h;

    goc_fs_event_ctx_t* ctx = (goc_fs_event_ctx_t*)goc_malloc(sizeof(goc_fs_event_ctx_t));
    ctx->ch = d->ch;
    d->handle->data = ctx;

    int rc = uv_fs_event_start(d->handle, on_fs_event_cb, d->path, d->flags);
    if (rc < 0) {
        goc_close(d->ch);
        d->handle->data = NULL;
    }

    uv_close((uv_handle_t*)h, unregister_io_handle);
}

goc_chan* goc_io_fs_event_start(uv_fs_event_t* handle, const char* path,
                                unsigned flags)
{
    goc_chan*                       ch = goc_chan_make(16);
    goc_fs_event_start_dispatch_t*  d  = (goc_fs_event_start_dispatch_t*)goc_malloc(
                                             sizeof(goc_fs_event_start_dispatch_t));
    size_t plen = strlen(path);
    d->handle = handle;
    d->path   = (char*)goc_malloc(plen + 1);
    memcpy(d->path, path, plen + 1);
    d->flags  = flags;
    d->ch     = ch;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_fs_event_start_dispatch, "goc_io_fs_event_start");
    return ch;
}

typedef struct {
    uv_async_t     async;   /* MUST be first member */
    uv_fs_event_t* handle;
} goc_fs_event_stop_dispatch_t;

static void on_fs_event_stop_dispatch(uv_async_t* h)
{
    goc_fs_event_stop_dispatch_t* d = (goc_fs_event_stop_dispatch_t*)h;
    uv_fs_event_stop(d->handle);
    if (d->handle->data) {
        goc_fs_event_ctx_t* ctx = (goc_fs_event_ctx_t*)d->handle->data;
        goc_close(ctx->ch);
        d->handle->data = NULL;
    }
    uv_close((uv_handle_t*)h, unregister_io_handle);
}

int goc_io_fs_event_stop(uv_fs_event_t* handle)
{
    goc_fs_event_stop_dispatch_t* d = (goc_fs_event_stop_dispatch_t*)goc_malloc(
                                          sizeof(goc_fs_event_stop_dispatch_t));
    d->handle = handle;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_fs_event_stop_dispatch, "goc_io_fs_event_stop");
    return 0;
}

/* =========================================================================
 * FS poll start / stop
 * ====================================================================== */

typedef struct {
    goc_chan* ch;
} goc_fs_poll_ctx_t;

static void on_fs_poll_cb(uv_fs_poll_t* handle, int status,
                          const uv_stat_t* prev, const uv_stat_t* curr)
{
    goc_fs_poll_ctx_t* ctx = (goc_fs_poll_ctx_t*)handle->data;
    if (!ctx || !ctx->ch) return;

    goc_io_fs_poll_t* res = (goc_io_fs_poll_t*)goc_malloc(sizeof(goc_io_fs_poll_t));
    res->ok = (status == 0) ? GOC_IO_OK : GOC_IO_ERR;
    if (prev) res->prev = *prev;
    if (curr) res->curr = *curr;
    goc_put_cb(ctx->ch, res, NULL, NULL);
}

typedef struct {
    uv_async_t     async;   /* MUST be first member */
    uv_fs_poll_t*  handle;
    char*          path;
    unsigned       interval_ms;
    goc_chan*      ch;
} goc_fs_poll_start_dispatch_t;

static void on_fs_poll_start_dispatch(uv_async_t* h)
{
    goc_fs_poll_start_dispatch_t* d = (goc_fs_poll_start_dispatch_t*)h;

    goc_fs_poll_ctx_t* ctx = (goc_fs_poll_ctx_t*)goc_malloc(sizeof(goc_fs_poll_ctx_t));
    ctx->ch = d->ch;
    d->handle->data = ctx;

    int rc = uv_fs_poll_start(d->handle, on_fs_poll_cb, d->path, d->interval_ms);
    if (rc < 0) {
        goc_close(d->ch);
        d->handle->data = NULL;
    }

    uv_close((uv_handle_t*)h, unregister_io_handle);
}

goc_chan* goc_io_fs_poll_start(uv_fs_poll_t* handle, const char* path,
                               unsigned interval_ms)
{
    goc_chan*                      ch = goc_chan_make(16);
    goc_fs_poll_start_dispatch_t*  d  = (goc_fs_poll_start_dispatch_t*)goc_malloc(
                                            sizeof(goc_fs_poll_start_dispatch_t));
    size_t plen = strlen(path);
    d->handle      = handle;
    d->path        = (char*)goc_malloc(plen + 1);
    memcpy(d->path, path, plen + 1);
    d->interval_ms = interval_ms;
    d->ch          = ch;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_fs_poll_start_dispatch, "goc_io_fs_poll_start");
    return ch;
}

typedef struct {
    uv_async_t    async;   /* MUST be first member */
    uv_fs_poll_t* handle;
} goc_fs_poll_stop_dispatch_t;

static void on_fs_poll_stop_dispatch(uv_async_t* h)
{
    goc_fs_poll_stop_dispatch_t* d = (goc_fs_poll_stop_dispatch_t*)h;
    uv_fs_poll_stop(d->handle);
    if (d->handle->data) {
        goc_fs_poll_ctx_t* ctx = (goc_fs_poll_ctx_t*)d->handle->data;
        goc_close(ctx->ch);
        d->handle->data = NULL;
    }
    uv_close((uv_handle_t*)h, unregister_io_handle);
}

int goc_io_fs_poll_stop(uv_fs_poll_t* handle)
{
    goc_fs_poll_stop_dispatch_t* d = (goc_fs_poll_stop_dispatch_t*)goc_malloc(
                                         sizeof(goc_fs_poll_stop_dispatch_t));
    d->handle = handle;
    gc_handle_register(d);
    dispatch_async_or_abort(&d->async, on_fs_poll_stop_dispatch, "goc_io_fs_poll_stop");
    return 0;
}

/* =========================================================================
 * Extended FS operations (all follow goc_fs_ctx_t pattern)
 * ====================================================================== */

/* Generic FS ops that deliver SCALAR(result). */
typedef struct {
    uv_fs_t   req;   /* MUST be first */
    goc_chan* ch;
} goc_fs_scalar_ctx_t;

static void on_fs_scalar(uv_fs_t* req)
{
    goc_fs_scalar_ctx_t* ctx = (goc_fs_scalar_ctx_t*)req;
    int result = (int)req->result;
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(result), close_on_put, ctx->ch);
}

/* Macro to define trivial scalar FS wrappers. */
#define DEF_FS_SCALAR(fname, uv_call)                                        \
    goc_chan* fname {                                                          \
        goc_chan*             ch  = goc_chan_make(1);                          \
        goc_fs_scalar_ctx_t* ctx = (goc_fs_scalar_ctx_t*)goc_malloc(          \
                                       sizeof(goc_fs_scalar_ctx_t));           \
        ctx->ch = ch;                                                          \
        int rc = uv_call;                                                      \
        if (rc < 0) { goc_put_cb(ch, SCALAR(rc), close_on_put, ch); return ch; } \
        gc_handle_register(ctx);                                               \
        return ch;                                                             \
    }

/* lstat */
static void on_fs_lstat(uv_fs_t* req)
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

goc_chan* goc_io_fs_lstat(const char* path)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_lstat(g_loop, &ctx->req, path, on_fs_lstat);
    if (rc < 0) {
        goc_io_fs_stat_t* res = (goc_io_fs_stat_t*)goc_malloc(sizeof(goc_io_fs_stat_t));
        res->ok = GOC_IO_ERR;
        goc_put_cb(ch, res, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

/* fstat */
static void on_fs_fstat(uv_fs_t* req)
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

goc_chan* goc_io_fs_fstat(uv_file file)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_fstat(g_loop, &ctx->req, file, on_fs_fstat);
    if (rc < 0) {
        goc_io_fs_stat_t* res = (goc_io_fs_stat_t*)goc_malloc(sizeof(goc_io_fs_stat_t));
        res->ok = GOC_IO_ERR;
        goc_put_cb(ch, res, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

/* ftruncate */
DEF_FS_SCALAR(goc_io_fs_ftruncate(uv_file file, int64_t offset),
              uv_fs_ftruncate(g_loop, &ctx->req, file, offset, on_fs_scalar))

/* access */
DEF_FS_SCALAR(goc_io_fs_access(const char* path, int mode),
              uv_fs_access(g_loop, &ctx->req, path, mode, on_fs_scalar))

/* chmod */
DEF_FS_SCALAR(goc_io_fs_chmod(const char* path, int mode),
              uv_fs_chmod(g_loop, &ctx->req, path, mode, on_fs_scalar))

/* fchmod */
DEF_FS_SCALAR(goc_io_fs_fchmod(uv_file file, int mode),
              uv_fs_fchmod(g_loop, &ctx->req, file, mode, on_fs_scalar))

/* chown */
DEF_FS_SCALAR(goc_io_fs_chown(const char* path, uv_uid_t uid, uv_gid_t gid),
              uv_fs_chown(g_loop, &ctx->req, path, uid, gid, on_fs_scalar))

/* fchown */
DEF_FS_SCALAR(goc_io_fs_fchown(uv_file file, uv_uid_t uid, uv_gid_t gid),
              uv_fs_fchown(g_loop, &ctx->req, file, uid, gid, on_fs_scalar))

/* utime */
DEF_FS_SCALAR(goc_io_fs_utime(const char* path, double atime, double mtime),
              uv_fs_utime(g_loop, &ctx->req, path, atime, mtime, on_fs_scalar))

/* futime */
DEF_FS_SCALAR(goc_io_fs_futime(uv_file file, double atime, double mtime),
              uv_fs_futime(g_loop, &ctx->req, file, atime, mtime, on_fs_scalar))

/* lutime */
DEF_FS_SCALAR(goc_io_fs_lutime(const char* path, double atime, double mtime),
              uv_fs_lutime(g_loop, &ctx->req, path, atime, mtime, on_fs_scalar))

/* mkdir */
DEF_FS_SCALAR(goc_io_fs_mkdir(const char* path, int mode),
              uv_fs_mkdir(g_loop, &ctx->req, path, mode, on_fs_scalar))

/* rmdir */
DEF_FS_SCALAR(goc_io_fs_rmdir(const char* path),
              uv_fs_rmdir(g_loop, &ctx->req, path, on_fs_scalar))

/* copyfile */
DEF_FS_SCALAR(goc_io_fs_copyfile(const char* src, const char* dst, int flags),
              uv_fs_copyfile(g_loop, &ctx->req, src, dst, flags, on_fs_scalar))

/* link */
DEF_FS_SCALAR(goc_io_fs_link(const char* path, const char* new_path),
              uv_fs_link(g_loop, &ctx->req, path, new_path, on_fs_scalar))

/* symlink */
DEF_FS_SCALAR(goc_io_fs_symlink(const char* path, const char* new_path, int flags),
              uv_fs_symlink(g_loop, &ctx->req, path, new_path, flags, on_fs_scalar))

/* fsync */
DEF_FS_SCALAR(goc_io_fs_fsync(uv_file file),
              uv_fs_fsync(g_loop, &ctx->req, file, on_fs_scalar))

/* fdatasync */
DEF_FS_SCALAR(goc_io_fs_fdatasync(uv_file file),
              uv_fs_fdatasync(g_loop, &ctx->req, file, on_fs_scalar))

/* -------------------------------------------------------------------------
 * goc_io_fs_truncate (path variant: open + ftruncate + close)
 * ---------------------------------------------------------------------- */

typedef enum {
    TRUNC_OPEN = 0,
    TRUNC_FTRUNCATE,
    TRUNC_CLOSE
} goc_trunc_state_t;

typedef struct {
    uv_fs_t           req;   /* MUST be first */
    goc_chan*         ch;
    int64_t           offset;
    goc_trunc_state_t state;
    uv_file           fd;
} goc_fs_truncate_ctx_t;

static void on_fs_truncate_step(uv_fs_t* req);

static void on_fs_truncate_step(uv_fs_t* req)
{
    goc_fs_truncate_ctx_t* ctx = (goc_fs_truncate_ctx_t*)req;
    int result = (int)req->result;
    uv_fs_req_cleanup(req);

    switch (ctx->state) {
        case TRUNC_OPEN:
            if (result < 0) {
                gc_handle_unregister(ctx);
                goc_put_cb(ctx->ch, SCALAR(result), close_on_put, ctx->ch);
                return;
            }
            ctx->fd    = (uv_file)result;
            ctx->state = TRUNC_FTRUNCATE;
            uv_fs_ftruncate(g_loop, &ctx->req, ctx->fd, ctx->offset,
                            on_fs_truncate_step);
            break;
        case TRUNC_FTRUNCATE: {
            int saved = result;
            ctx->state = TRUNC_CLOSE;
            /* Store result in offset field temporarily to pass through close. */
            ctx->offset = (int64_t)saved;
            uv_fs_close(g_loop, &ctx->req, ctx->fd, on_fs_truncate_step);
            break;
        }
        case TRUNC_CLOSE: {
            int final_rc = (int)ctx->offset; /* restored */
            gc_handle_unregister(ctx);
            goc_put_cb(ctx->ch, SCALAR(final_rc), close_on_put, ctx->ch);
            break;
        }
    }
}

goc_chan* goc_io_fs_truncate(const char* path, int64_t offset)
{
    goc_chan*               ch  = goc_chan_make(1);
    goc_fs_truncate_ctx_t*  ctx = (goc_fs_truncate_ctx_t*)goc_malloc(
                                      sizeof(goc_fs_truncate_ctx_t));
    ctx->ch     = ch;
    ctx->offset = offset;
    ctx->state  = TRUNC_OPEN;
    ctx->fd     = -1;

    int rc = uv_fs_open(g_loop, &ctx->req, path,
                        UV_FS_O_WRONLY, 0, on_fs_truncate_step);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

/* -------------------------------------------------------------------------
 * goc_io_fs_readdir
 * ---------------------------------------------------------------------- */

static void on_fs_readdir(uv_fs_t* req)
{
    goc_fs_ctx_t*        ctx = (goc_fs_ctx_t*)req;
    goc_io_fs_readdir_t* res = (goc_io_fs_readdir_t*)goc_malloc(
                                   sizeof(goc_io_fs_readdir_t));
    if (req->result < 0) {
        res->ok      = GOC_IO_ERR;
        res->entries = goc_array_make(0);
        uv_fs_req_cleanup(req);
        gc_handle_unregister(ctx);
        goc_put_cb(ctx->ch, res, close_on_put, ctx->ch);
        return;
    }

    res->ok      = GOC_IO_OK;
    res->entries = goc_array_make((size_t)req->result);

    uv_dirent_t dent;
    while (uv_fs_scandir_next(req, &dent) != UV_EOF) {
        goc_io_fs_dirent_t* ent = (goc_io_fs_dirent_t*)goc_malloc(
                                      sizeof(goc_io_fs_dirent_t));
        size_t nlen = strlen(dent.name);
        char*  nbuf = (char*)goc_malloc(nlen + 1);
        memcpy(nbuf, dent.name, nlen + 1);
        ent->name = nbuf;
        ent->type = dent.type;
        goc_array_push(res->entries, ent);
    }

    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, res, close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_readdir(const char* path)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_scandir(g_loop, &ctx->req, path, 0, on_fs_readdir);
    if (rc < 0) {
        goc_io_fs_readdir_t* res = (goc_io_fs_readdir_t*)goc_malloc(
                                       sizeof(goc_io_fs_readdir_t));
        res->ok      = GOC_IO_ERR;
        res->entries = goc_array_make(0);
        goc_put_cb(ch, res, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

/* -------------------------------------------------------------------------
 * goc_io_fs_mkdtemp
 * ---------------------------------------------------------------------- */

static void on_fs_mkdtemp(uv_fs_t* req)
{
    goc_fs_ctx_t*      ctx = (goc_fs_ctx_t*)req;
    goc_io_fs_path_t*  res = (goc_io_fs_path_t*)goc_malloc(sizeof(goc_io_fs_path_t));
    if (req->result == 0 && req->path) {
        size_t plen = strlen(req->path);
        char*  pbuf = (char*)goc_malloc(plen + 1);
        memcpy(pbuf, req->path, plen + 1);
        res->ok   = GOC_IO_OK;
        res->path = pbuf;
    } else {
        res->ok   = GOC_IO_ERR;
        res->path = NULL;
    }
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, res, close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_mkdtemp(const char* tpl)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_mkdtemp(g_loop, &ctx->req, tpl, on_fs_mkdtemp);
    if (rc < 0) {
        goc_io_fs_path_t* res = (goc_io_fs_path_t*)goc_malloc(sizeof(goc_io_fs_path_t));
        res->ok   = GOC_IO_ERR;
        res->path = NULL;
        goc_put_cb(ch, res, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

/* -------------------------------------------------------------------------
 * goc_io_fs_mkstemp
 * ---------------------------------------------------------------------- */

static void on_fs_mkstemp(uv_fs_t* req)
{
    goc_fs_ctx_t*        ctx = (goc_fs_ctx_t*)req;
    goc_io_fs_mkstemp_t* res = (goc_io_fs_mkstemp_t*)goc_malloc(
                                   sizeof(goc_io_fs_mkstemp_t));
    if (req->result >= 0) {
        res->ok = GOC_IO_OK;
        res->fd = (uv_file)req->result;
        if (req->path) {
            size_t plen = strlen(req->path);
            char*  pbuf = (char*)goc_malloc(plen + 1);
            memcpy(pbuf, req->path, plen + 1);
            res->path = pbuf;
        } else {
            res->path = NULL;
        }
    } else {
        res->ok   = GOC_IO_ERR;
        res->fd   = -1;
        res->path = NULL;
    }
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, res, close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_mkstemp(const char* tpl)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_mkstemp(g_loop, &ctx->req, tpl, on_fs_mkstemp);
    if (rc < 0) {
        goc_io_fs_mkstemp_t* res = (goc_io_fs_mkstemp_t*)goc_malloc(
                                       sizeof(goc_io_fs_mkstemp_t));
        res->ok   = GOC_IO_ERR;
        res->fd   = -1;
        res->path = NULL;
        goc_put_cb(ch, res, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

/* -------------------------------------------------------------------------
 * goc_io_fs_readlink / goc_io_fs_realpath — deliver goc_io_fs_path_t*
 * ---------------------------------------------------------------------- */

static void on_fs_path(uv_fs_t* req)
{
    goc_fs_ctx_t*     ctx = (goc_fs_ctx_t*)req;
    goc_io_fs_path_t* res = (goc_io_fs_path_t*)goc_malloc(sizeof(goc_io_fs_path_t));
    if (req->result == 0 && req->ptr) {
        const char* p    = (const char*)req->ptr;
        size_t      plen = strlen(p);
        char*       pbuf = (char*)goc_malloc(plen + 1);
        memcpy(pbuf, p, plen + 1);
        res->ok   = GOC_IO_OK;
        res->path = pbuf;
    } else {
        res->ok   = GOC_IO_ERR;
        res->path = NULL;
    }
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, res, close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_readlink(const char* path)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_readlink(g_loop, &ctx->req, path, on_fs_path);
    if (rc < 0) {
        goc_io_fs_path_t* res = (goc_io_fs_path_t*)goc_malloc(sizeof(goc_io_fs_path_t));
        res->ok   = GOC_IO_ERR;
        res->path = NULL;
        goc_put_cb(ch, res, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

goc_chan* goc_io_fs_realpath(const char* path)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_realpath(g_loop, &ctx->req, path, on_fs_path);
    if (rc < 0) {
        goc_io_fs_path_t* res = (goc_io_fs_path_t*)goc_malloc(sizeof(goc_io_fs_path_t));
        res->ok   = GOC_IO_ERR;
        res->path = NULL;
        goc_put_cb(ch, res, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

/* -------------------------------------------------------------------------
 * goc_io_fs_statfs
 * ---------------------------------------------------------------------- */

static void on_fs_statfs(uv_fs_t* req)
{
    goc_fs_ctx_t*        ctx = (goc_fs_ctx_t*)req;
    goc_io_fs_statfs_t*  res = (goc_io_fs_statfs_t*)goc_malloc(
                                   sizeof(goc_io_fs_statfs_t));
    if (req->result == 0 && req->ptr) {
        res->ok     = GOC_IO_OK;
        res->statbuf = *(uv_statfs_t*)req->ptr;
    } else {
        res->ok = GOC_IO_ERR;
        memset(&res->statbuf, 0, sizeof(res->statbuf));
    }
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, res, close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_statfs(const char* path)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_statfs(g_loop, &ctx->req, path, on_fs_statfs);
    if (rc < 0) {
        goc_io_fs_statfs_t* res = (goc_io_fs_statfs_t*)goc_malloc(
                                      sizeof(goc_io_fs_statfs_t));
        res->ok = GOC_IO_ERR;
        memset(&res->statbuf, 0, sizeof(res->statbuf));
        goc_put_cb(ch, res, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

/* =========================================================================
 * High-level file helpers
 * ====================================================================== */

/* -------------------------------------------------------------------------
 * goc_io_fs_read_file
 *
 * State machine: OPEN -> READ_LOOP -> CLOSE
 * ---------------------------------------------------------------------- */

#define READ_FILE_CHUNK  65536

typedef enum {
    RF_OPEN = 0,
    RF_READ,
    RF_CLOSE
} goc_read_file_state_t;

typedef struct {
    uv_fs_t              req;    /* MUST be first */
    goc_chan*            ch;
    goc_read_file_state_t state;
    uv_file              fd;
    char*                data;   /* accumulator (goc_malloc'd, null-terminated) */
    size_t               data_len;
    char*                raw;    /* goc_malloc'd read buffer */
    int                  error;  /* saved error from read phase */
} goc_read_file_ctx_t;

static void on_read_file_step(uv_fs_t* req);

static void on_read_file_step(uv_fs_t* req)
{
    goc_read_file_ctx_t* ctx = (goc_read_file_ctx_t*)req;
    uv_fs_req_cleanup(req);

    switch (ctx->state) {
        case RF_OPEN:
            if (req->result < 0) {
                gc_handle_unregister(ctx);
                goc_io_fs_read_file_t* res = (goc_io_fs_read_file_t*)goc_malloc(
                                                 sizeof(goc_io_fs_read_file_t));
                res->ok   = GOC_IO_ERR;
                res->data = NULL;
                goc_put_cb(ctx->ch, res, close_on_put, ctx->ch);
                return;
            }
            ctx->fd    = (uv_file)req->result;
            ctx->state = RF_READ;
            /* fall through to start first read */
            {
                uv_buf_t uvbuf = uv_buf_init(ctx->raw, READ_FILE_CHUNK);
                uv_fs_read(g_loop, &ctx->req, ctx->fd, &uvbuf, 1, -1,
                           on_read_file_step);
            }
            break;

        case RF_READ:
            if (req->result < 0) {
                /* Error: save and close file. */
                ctx->error = (int)req->result;
                ctx->state = RF_CLOSE;
                uv_fs_close(g_loop, &ctx->req, ctx->fd, on_read_file_step);
            } else if (req->result == 0) {
                /* EOF: close file. */
                ctx->state = RF_CLOSE;
                uv_fs_close(g_loop, &ctx->req, ctx->fd, on_read_file_step);
            } else {
                /* Got data: append to accumulator and read again. */
                ssize_t n = (ssize_t)req->result;
                char* newdata = (char*)goc_malloc(ctx->data_len + (size_t)n + 1);
                memcpy(newdata, ctx->data, ctx->data_len);
                memcpy(newdata + ctx->data_len, ctx->raw, (size_t)n);
                ctx->data_len += (size_t)n;
                newdata[ctx->data_len] = '\0';
                ctx->data = newdata;
                uv_buf_t uvbuf = uv_buf_init(ctx->raw, READ_FILE_CHUNK);
                uv_fs_read(g_loop, &ctx->req, ctx->fd, &uvbuf, 1, -1,
                           on_read_file_step);
            }
            break;

        case RF_CLOSE: {
            gc_handle_unregister(ctx);
            goc_io_fs_read_file_t* res = (goc_io_fs_read_file_t*)goc_malloc(
                                             sizeof(goc_io_fs_read_file_t));
            if (ctx->error) {
                res->ok   = GOC_IO_ERR;
                res->data = NULL;
            } else {
                res->ok   = GOC_IO_OK;
                res->data = ctx->data;
            }
            goc_put_cb(ctx->ch, res, close_on_put, ctx->ch);
            break;
        }
    }
}

goc_chan* goc_io_fs_read_file(const char* path)
{
    goc_chan*             ch  = goc_chan_make(1);
    goc_read_file_ctx_t*  ctx = (goc_read_file_ctx_t*)goc_malloc(
                                    sizeof(goc_read_file_ctx_t));
    ctx->ch       = ch;
    ctx->state    = RF_OPEN;
    ctx->fd       = -1;
    ctx->data     = (char*)goc_malloc(1);
    ctx->data[0]  = '\0';
    ctx->data_len = 0;
    ctx->raw      = (char*)goc_malloc(READ_FILE_CHUNK);
    ctx->error = 0;

    int rc = uv_fs_open(g_loop, &ctx->req, path, UV_FS_O_RDONLY, 0,
                        on_read_file_step);
    if (rc < 0) {
        goc_io_fs_read_file_t* res = (goc_io_fs_read_file_t*)goc_malloc(
                                         sizeof(goc_io_fs_read_file_t));
        res->ok   = GOC_IO_ERR;
        res->data = NULL;
        goc_put_cb(ch, res, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

/* -------------------------------------------------------------------------
 * goc_io_fs_write_file / goc_io_fs_append_file
 *
 * State machine: OPEN -> WRITE -> CLOSE
 * ---------------------------------------------------------------------- */

typedef enum {
    WF_OPEN = 0,
    WF_WRITE,
    WF_CLOSE
} goc_write_file_state_t;

typedef struct {
    uv_fs_t              req;    /* MUST be first */
    goc_chan*            ch;
    goc_write_file_state_t state;
    uv_file              fd;
    char*                raw;    /* the data bytes */
    size_t               len;
    int                  error;
} goc_write_file_ctx_t;

static void on_write_file_step(uv_fs_t* req);

static void on_write_file_step(uv_fs_t* req)
{
    goc_write_file_ctx_t* ctx = (goc_write_file_ctx_t*)req;
    uv_fs_req_cleanup(req);

    switch (ctx->state) {
        case WF_OPEN:
            if (req->result < 0) {
                gc_handle_unregister(ctx);
                goc_put_cb(ctx->ch, SCALAR((int)req->result), close_on_put, ctx->ch);
                return;
            }
            ctx->fd    = (uv_file)req->result;
            ctx->state = WF_WRITE;
            {
                uv_buf_t uvbuf = uv_buf_init(ctx->raw, (unsigned int)ctx->len);
                uv_fs_write(g_loop, &ctx->req, ctx->fd, &uvbuf, 1, -1,
                            on_write_file_step);
            }
            break;

        case WF_WRITE:
            ctx->error = (req->result < 0) ? (int)req->result : 0;
            ctx->state = WF_CLOSE;
            uv_fs_close(g_loop, &ctx->req, ctx->fd, on_write_file_step);
            break;

        case WF_CLOSE:
            gc_handle_unregister(ctx);
            goc_put_cb(ctx->ch, SCALAR(ctx->error), close_on_put, ctx->ch);
            break;
    }
}

static goc_chan* write_file_impl(const char* path, const char* data,
                                 int open_flags)
{
    goc_chan*              ch  = goc_chan_make(1);
    size_t                 len = strlen(data);
    goc_write_file_ctx_t*  ctx = (goc_write_file_ctx_t*)goc_malloc(
                                     sizeof(goc_write_file_ctx_t));
    ctx->ch    = ch;
    ctx->state = WF_OPEN;
    ctx->fd    = -1;
    ctx->error = 0;
    ctx->len   = len;
    ctx->raw   = (char*)goc_malloc(len + 1);
    memcpy(ctx->raw, data, len);

    int rc = uv_fs_open(g_loop, &ctx->req, path, open_flags, 0644,
                        on_write_file_step);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

goc_chan* goc_io_fs_write_file(const char* path, const char* data, int flags)
{
    return write_file_impl(path, data, flags);
}

goc_chan* goc_io_fs_append_file(const char* path, const char* data)
{
    return write_file_impl(path, data,
                           UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_APPEND);
}

/* -------------------------------------------------------------------------
 * goc_io_fs_read_stream_make
 * ---------------------------------------------------------------------- */

typedef enum {
    RS_OPEN = 0,
    RS_READ,
    RS_DONE
} goc_read_stream_state_t;

typedef struct {
    uv_fs_t               req;    /* MUST be first */
    goc_chan*             ch;
    goc_read_stream_state_t state;
    uv_file               fd;
    char*                 raw;
    size_t                chunk_size;
} goc_read_stream_ctx_t;

static void on_read_stream_step(uv_fs_t* req);

static void on_read_stream_step(uv_fs_t* req)
{
    goc_read_stream_ctx_t* ctx = (goc_read_stream_ctx_t*)req;
    uv_fs_req_cleanup(req);

    switch (ctx->state) {
        case RS_OPEN:
            if (req->result < 0) {
                gc_handle_unregister(ctx);
                goc_io_fs_read_chunk_t* chunk = (goc_io_fs_read_chunk_t*)goc_malloc(
                                                    sizeof(goc_io_fs_read_chunk_t));
                chunk->status = (int)req->result;
                chunk->data   = NULL;
                chunk->len    = 0;
                goc_put_cb(ctx->ch, chunk, close_on_put, ctx->ch);
                return;
            }
            ctx->fd    = (uv_file)req->result;
            ctx->state = RS_READ;
            {
                uv_buf_t uvbuf = uv_buf_init(ctx->raw, (unsigned int)ctx->chunk_size);
                uv_fs_read(g_loop, &ctx->req, ctx->fd, &uvbuf, 1, -1,
                           on_read_stream_step);
            }
            break;

        case RS_READ:
            if (req->result < 0) {
                /* Error: close file and deliver error chunk. */
                int err = (int)req->result;
                /* Close synchronously-ish by reusing req for close, then deliver. */
                uv_fs_t close_req;
                uv_fs_close(NULL, &close_req, ctx->fd, NULL);
                uv_fs_req_cleanup(&close_req);
                gc_handle_unregister(ctx);
                goc_io_fs_read_chunk_t* chunk = (goc_io_fs_read_chunk_t*)goc_malloc(
                                                    sizeof(goc_io_fs_read_chunk_t));
                chunk->status = err;
                chunk->data   = NULL;
                chunk->len    = 0;
                goc_put_cb(ctx->ch, chunk, close_on_put, ctx->ch);
            } else if (req->result == 0) {
                /* EOF: close file and close channel. */
                uv_fs_t close_req;
                uv_fs_close(NULL, &close_req, ctx->fd, NULL);
                uv_fs_req_cleanup(&close_req);
                gc_handle_unregister(ctx);
                goc_close(ctx->ch);
            } else {
                /* Deliver chunk and read again. */
                ssize_t n = (ssize_t)req->result;
                char* buf = (char*)goc_malloc((size_t)n);
                memcpy(buf, ctx->raw, (size_t)n);

                goc_io_fs_read_chunk_t* chunk = (goc_io_fs_read_chunk_t*)goc_malloc(
                                                    sizeof(goc_io_fs_read_chunk_t));
                chunk->status = 0;
                chunk->data   = buf;
                chunk->len    = (size_t)n;
                goc_put_cb(ctx->ch, chunk, NULL, NULL);

                uv_buf_t uvbuf = uv_buf_init(ctx->raw,
                                             (unsigned int)ctx->chunk_size);
                uv_fs_read(g_loop, &ctx->req, ctx->fd, &uvbuf, 1, -1,
                           on_read_stream_step);
            }
            break;

        case RS_DONE:
            break;
    }
}

goc_chan* goc_io_fs_read_stream_make(const char* path, size_t chunk_size)
{
    goc_chan*               ch  = goc_chan_make(16);
    goc_read_stream_ctx_t*  ctx = (goc_read_stream_ctx_t*)goc_malloc(
                                      sizeof(goc_read_stream_ctx_t));
    ctx->ch         = ch;
    ctx->state      = RS_OPEN;
    ctx->fd         = -1;
    ctx->chunk_size = chunk_size ? chunk_size : 65536;
    ctx->raw        = (char*)goc_malloc(ctx->chunk_size);

    int rc = uv_fs_open(g_loop, &ctx->req, path, UV_FS_O_RDONLY, 0,
                        on_read_stream_step);
    if (rc < 0) {
        goc_io_fs_read_chunk_t* chunk = (goc_io_fs_read_chunk_t*)goc_malloc(
                                            sizeof(goc_io_fs_read_chunk_t));
        chunk->status = rc;
        chunk->data   = NULL;
        goc_put_cb(ch, chunk, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

/* -------------------------------------------------------------------------
 * goc_io_fs_write_stream_make / write / end
 * ---------------------------------------------------------------------- */

/* Concrete definition (opaque to users via forward declaration in header). */
struct goc_io_fs_write_stream {
    uv_file fd;
};

typedef struct {
    uv_fs_t                   req;    /* MUST be first */
    goc_chan*                 ch;
    goc_io_fs_write_stream_t* ws;
} goc_ws_open_ctx_t;

static void on_ws_open(uv_fs_t* req)
{
    goc_ws_open_ctx_t* ctx = (goc_ws_open_ctx_t*)req;
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);

    goc_io_fs_write_stream_open_t* res = (goc_io_fs_write_stream_open_t*)goc_malloc(
                                             sizeof(goc_io_fs_write_stream_open_t));
    if (req->result < 0) {
        res->ok = GOC_IO_ERR;
        res->ws = NULL;
    } else {
        ctx->ws->fd = (uv_file)req->result;
        res->ok     = GOC_IO_OK;
        res->ws     = ctx->ws;
    }
    goc_put_cb(ctx->ch, res, close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_write_stream_make(const char* path, int flags)
{
    goc_chan*                  ch  = goc_chan_make(1);
    goc_io_fs_write_stream_t*  ws  = (goc_io_fs_write_stream_t*)goc_malloc(
                                         sizeof(goc_io_fs_write_stream_t));
    ws->fd = -1;

    goc_ws_open_ctx_t* ctx = (goc_ws_open_ctx_t*)goc_malloc(sizeof(goc_ws_open_ctx_t));
    ctx->ch = ch;
    ctx->ws = ws;

    int rc = uv_fs_open(g_loop, &ctx->req, path, flags, 0644, on_ws_open);
    if (rc < 0) {
        goc_io_fs_write_stream_open_t* res = (goc_io_fs_write_stream_open_t*)goc_malloc(
                                                 sizeof(goc_io_fs_write_stream_open_t));
        res->ok = GOC_IO_ERR;
        res->ws = NULL;
        goc_put_cb(ch, res, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

typedef struct {
    uv_fs_t   req;    /* MUST be first */
    goc_chan* ch;
    char*     raw;
} goc_ws_write_ctx_t;

static void on_ws_write(uv_fs_t* req)
{
    goc_ws_write_ctx_t* ctx = (goc_ws_write_ctx_t*)req;
    ssize_t result = (ssize_t)req->result;
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(result), close_on_put, ctx->ch);
}

goc_chan* goc_io_fs_write_stream_write(goc_io_fs_write_stream_t* ws,
                                       const char* data, size_t len)
{
    goc_chan*            ch  = goc_chan_make(1);
    goc_ws_write_ctx_t*  ctx = (goc_ws_write_ctx_t*)goc_malloc(
                                   sizeof(goc_ws_write_ctx_t));
    ctx->ch  = ch;
    ctx->raw = (char*)goc_malloc(len + 1);
    memcpy(ctx->raw, data, len);

    uv_buf_t uvbuf = uv_buf_init(ctx->raw, (unsigned int)len);
    int rc = uv_fs_write(g_loop, &ctx->req, ws->fd, &uvbuf, 1, -1, on_ws_write);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

typedef enum {
    WE_FSYNC = 0,
    WE_CLOSE
} goc_ws_end_state_t;

typedef struct {
    uv_fs_t           req;    /* MUST be first */
    goc_chan*         ch;
    uv_file           fd;
    goc_ws_end_state_t state;
    int               error;
} goc_ws_end_ctx_t;

static void on_ws_end_step(uv_fs_t* req);

static void on_ws_end_step(uv_fs_t* req)
{
    goc_ws_end_ctx_t* ctx = (goc_ws_end_ctx_t*)req;
    uv_fs_req_cleanup(req);

    switch (ctx->state) {
        case WE_FSYNC:
            ctx->error = (req->result < 0) ? (int)req->result : 0;
            ctx->state = WE_CLOSE;
            uv_fs_close(g_loop, &ctx->req, ctx->fd, on_ws_end_step);
            break;
        case WE_CLOSE:
            gc_handle_unregister(ctx);
            goc_put_cb(ctx->ch, SCALAR(ctx->error), close_on_put, ctx->ch);
            break;
    }
}

goc_chan* goc_io_fs_write_stream_end(goc_io_fs_write_stream_t* ws)
{
    goc_chan*          ch  = goc_chan_make(1);
    goc_ws_end_ctx_t*  ctx = (goc_ws_end_ctx_t*)goc_malloc(sizeof(goc_ws_end_ctx_t));
    ctx->ch    = ch;
    ctx->fd    = ws->fd;
    ctx->state = WE_FSYNC;
    ctx->error = 0;

    int rc = uv_fs_fsync(g_loop, &ctx->req, ws->fd, on_ws_end_step);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

/* =========================================================================
 * goc_io_random
 * ====================================================================== */

typedef struct {
    uv_random_t   req;    /* MUST be first */
    goc_chan*     ch;
    char*         raw;
    size_t        n;
} goc_random_ctx_t;

static void on_random(uv_random_t* req, int status, void* buf, size_t buflen)
{
    goc_random_ctx_t* ctx = (goc_random_ctx_t*)req;
    gc_handle_unregister(ctx);

    goc_io_random_t* res = (goc_io_random_t*)goc_malloc(sizeof(goc_io_random_t));
    if (status == 0) {
        goc_array* arr = goc_array_make(buflen);
        size_t i;
        for (i = 0; i < buflen; i++)
            goc_array_push(arr, goc_box_int((unsigned char)ctx->raw[i]));
        res->ok   = GOC_IO_OK;
        res->data = arr;
    } else {
        res->ok   = GOC_IO_ERR;
        res->data = NULL;
    }
    (void)buf;
    goc_put_cb(ctx->ch, res, close_on_put, ctx->ch);
}

goc_chan* goc_io_random(size_t n, unsigned flags)
{
    goc_chan*          ch  = goc_chan_make(1);
    goc_random_ctx_t*  ctx = (goc_random_ctx_t*)goc_malloc(sizeof(goc_random_ctx_t));
    ctx->ch  = ch;
    ctx->n   = n;
    ctx->raw = (char*)goc_malloc(n + 1);

    int rc = uv_random(g_loop, &ctx->req, ctx->raw, n, flags, on_random);
    if (rc < 0) {
        goc_io_random_t* res = (goc_io_random_t*)goc_malloc(sizeof(goc_io_random_t));
        res->ok   = GOC_IO_ERR;
        res->data = NULL;
        goc_put_cb(ch, res, close_on_put, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}
