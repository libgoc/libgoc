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
 * thread-safe.  These must run on the event loop thread.
 *
 * The core TCP I/O path (read_start/stop, write, tcp_connect, handle_init/
 * close, and tcp_server_make) uses post_on_loop(): a GOC_CALLBACK entry is
 * enqueued directly into the MPSC callback queue so no extra uv_async_t
 * handle is allocated per call.
 *
 * All remaining handle operations (write2, shutdown, pipe_connect, UDP,
 * signals, process spawn, TTY, FS events/polls) also use post_on_loop().
 *
 * Result delivery
 * ---------------
 * All one-shot callbacks deliver their result via goc_put_cb() with a
 * goc_close_cb completion callback.  goc_put_cb() is non-blocking:
 * it posts the put to the MPSC queue for the loop thread to process.  The
 * loop thread delivers the value to any parked fiber taker (or buffers it)
 * and then fires goc_close.
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
#include <signal.h>
#include <assert.h>
#include <stdatomic.h>
#ifndef _WIN32
# include <sys/socket.h>
#endif
#include <uv.h>
#include <gc.h>
#include "../include/goc_io.h"
#include "../include/goc_array.h"
#include "internal.h"
#include "channel_internal.h"

struct goc_io_handle {
    uint32_t                magic;
    uv_handle_t*            uv;
    void*                   owner;
    _Atomic goc_io_handle_state_t state;
};

static bool goc_io_handle_claim_close(goc_io_handle_t* h);
static void drop_buffered_accept_connections(goc_chan* ch);
static void close_chan_if_not_closing(goc_chan* ch);

/* =========================================================================
 * Shared helpers
 * ====================================================================== */

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

/* Dispatch fn(arg) to the loop that owns handle_loop.
 * - Same worker as handle owner → direct call (we ARE the loop thread).
 * - Different worker loop       → post_on_handle_loop (MPSC task queue).
 * - g_loop                      → post_on_loop as before.
 */
static void dispatch_on_handle_loop(uv_loop_t* handle_loop,
                                    void (*fn)(void*), void* arg)
{
    uv_loop_t* wloop = goc_current_worker_loop();
    int worker_id = goc_current_worker_id();
    GOC_DBG(
            "dispatch_on_handle_loop: target_loop=%p current_worker_loop=%p current_worker=%d target_is_g_loop=%d uv_loop_alive=%d fn=%p arg=%p\n",
            (void*)handle_loop,
            (void*)wloop,
            worker_id,
            handle_loop == g_loop,
            handle_loop ? uv_loop_alive(handle_loop) : -1,
            (void*)fn,
            arg);
    if (!handle_loop) {
        GOC_DBG("dispatch_on_handle_loop: target_loop=NULL current_worker=%d fn=%p arg=%p skipping\n",
                worker_id, (void*)fn, arg);
        free(arg);
        return;
    }
    if (wloop && wloop == handle_loop) {
        if (goc_current_worker_has_pending_tasks()) {
            GOC_DBG(
                    "dispatch_on_handle_loop: same worker loop %p (worker=%d) has pending tasks, falling back to post_on_handle_loop fn=%p arg=%p\n",
                    (void*)handle_loop, worker_id, (void*)fn, arg);
        } else {
            GOC_DBG(
                    "dispatch_on_handle_loop: direct call on worker loop %p (worker=%d) fn=%p arg=%p\n",
                    (void*)handle_loop, worker_id, (void*)fn, arg);
            fn(arg);
            GOC_DBG(
                    "dispatch_on_handle_loop: direct call completed on worker loop %p (worker=%d) fn=%p arg=%p\n",
                    (void*)handle_loop, worker_id, (void*)fn, arg);
            return;
        }
    }
    if (handle_loop != g_loop) {
        GOC_DBG(
                "dispatch_on_handle_loop: posting to worker loop %p (current_worker=%d) fn=%p arg=%p\n",
                (void*)handle_loop, worker_id, (void*)fn, arg);
        int rc = post_on_handle_loop(handle_loop, fn, arg);
        if (rc < 0) {
            GOC_DBG(
                    "dispatch_on_handle_loop: rejected post_on_handle_loop loop=%p fn=%p arg=%p rc=%d uv_loop_alive=%d current_worker=%d\n",
                    (void*)handle_loop, (void*)fn, arg, rc,
                    handle_loop ? uv_loop_alive(handle_loop) : -1,
                    worker_id);
            free(arg);
        } else {
            GOC_DBG(
                    "dispatch_on_handle_loop: post_on_handle_loop succeeded loop=%p fn=%p arg=%p rc=%d current_worker=%d\n",
                    (void*)handle_loop, (void*)fn, arg, rc, worker_id);
        }
    } else {
        GOC_DBG(
                "dispatch_on_handle_loop: posting to g_loop %p (current_worker=%d) fn=%p arg=%p\n",
                (void*)g_loop, worker_id, (void*)fn, arg);
        int rc = post_on_loop_checked(fn, arg);
        if (rc < 0) {
            GOC_DBG(
                    "dispatch_on_handle_loop: rejected post_on_loop_checked fn=%p arg=%p rc=%d g_loop_shutting_down=%d\n",
                    (void*)fn, arg, rc,
                    goc_loop_is_shutting_down());
            free(arg);
        } else {
            GOC_DBG(
                    "dispatch_on_handle_loop: post_on_loop_checked succeeded fn=%p arg=%p rc=%d g_loop_shutting_down=%d\n",
                    (void*)fn, arg, rc, goc_loop_is_shutting_down());
        }
    }
}

/* -------------------------------------------------------------------------
 * goc_io_fs_open
 * ---------------------------------------------------------------------- */
/**
 * goc_io_fs_open — Initiate an async file open; return result channel.
 *
 * path  : path of the file to open.
 * flags : open flags.
 * mode  : file mode.
 *
 * Returns a channel delivering the file descriptor on success.
 * On error, the channel delivers a scalar error code.
 */

static void on_fs_open(uv_fs_t* req)
{
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)req;
    uv_file fd = (uv_file)req->result;
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(fd), goc_close_cb, ctx->ch);
}

goc_chan* goc_io_fs_open(const char* path, int flags, int mode)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_open(goc_worker_or_default_loop(), &ctx->req, path, flags, mode, on_fs_open);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), goc_close_cb, ch);
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
    goc_put_cb(ctx->ch, SCALAR(result), goc_close_cb, ctx->ch);
}

goc_chan* goc_io_fs_close(uv_file file)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_close(goc_worker_or_default_loop(), &ctx->req, file, on_fs_close);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), goc_close_cb, ch);
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

    goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
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
    int rc = uv_fs_read(goc_worker_or_default_loop(), &ctx->req, file, &uvbuf, 1, offset, on_fs_read);
    if (rc < 0) {
        goc_io_fs_read_t* res = (goc_io_fs_read_t*)goc_malloc(sizeof(goc_io_fs_read_t));
        res->nread = rc;
        res->buf   = NULL;
        goc_put_cb(ch, res, goc_close_cb, ch);
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
    goc_put_cb(ctx->ch, SCALAR(result), goc_close_cb, ctx->ch);
}

goc_chan* goc_io_fs_write(uv_file file, const char* data, size_t len, int64_t offset)
{
    goc_chan*            ch  = goc_chan_make(1);
    goc_fs_write_ctx_t*  ctx = (goc_fs_write_ctx_t*)goc_malloc(
                                   sizeof(goc_fs_write_ctx_t));
    ctx->ch  = ch;
    ctx->raw = (char*)goc_malloc(len + 1);
    memcpy(ctx->raw, data, len);

    uv_buf_t uvbuf = uv_buf_init(ctx->raw, (unsigned int)len);
    int rc = uv_fs_write(goc_worker_or_default_loop(), &ctx->req, file, &uvbuf, 1, offset, on_fs_write);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), goc_close_cb, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_fs_unlink
 * ---------------------------------------------------------------------- */
/**
 * goc_io_fs_unlink — Initiate an async file deletion; return result channel.
 *
 * path : path of the file to delete.
 *
 * Returns a channel delivering 0 on success, or a negative libuv error code on failure.
 * On error, the channel delivers a scalar error code.
 */

static void on_fs_unlink(uv_fs_t* req)
{
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)req;
    int result = (int)req->result;
    uv_fs_req_cleanup(req);
    gc_handle_unregister(ctx);
    goc_put_cb(ctx->ch, SCALAR(result), goc_close_cb, ctx->ch);
}

goc_chan* goc_io_fs_unlink(const char* path)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_unlink(goc_worker_or_default_loop(), &ctx->req, path, on_fs_unlink);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), goc_close_cb, ch);
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
    goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
}

goc_chan* goc_io_fs_stat(const char* path)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_stat(goc_worker_or_default_loop(), &ctx->req, path, on_fs_stat);
    if (rc < 0) {
        goc_io_fs_stat_t* res = (goc_io_fs_stat_t*)goc_malloc(sizeof(goc_io_fs_stat_t));
        res->ok = GOC_IO_ERR;
        goc_put_cb(ch, res, goc_close_cb, ch);
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
    goc_put_cb(ctx->ch, SCALAR(result), goc_close_cb, ctx->ch);
}

goc_chan* goc_io_fs_rename(const char* path, const char* new_path)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_rename(goc_worker_or_default_loop(), &ctx->req, path, new_path, on_fs_rename);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), goc_close_cb, ch);
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
    goc_put_cb(ctx->ch, SCALAR(result), goc_close_cb, ctx->ch);
}

goc_chan* goc_io_fs_sendfile(uv_file out_fd, uv_file in_fd,
                                int64_t in_offset, size_t length)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_sendfile(goc_worker_or_default_loop(), &ctx->req, out_fd, in_fd, in_offset,
                            length, on_fs_sendfile);
    if (rc < 0) {
        goc_put_cb(ch, SCALAR(rc), goc_close_cb, ch);
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
    long             submit_seq;
} goc_getaddrinfo_ctx_t;

static void dns_trace_submit(goc_getaddrinfo_ctx_t* ctx,
                             const char* node,
                             const char* service)
{
    GOC_DBG("[DNS_TRACE] submit req=%p ctx=%p ch=%p node=%s service=%s loop=%p shutdown=%d\n",
            (void*)&ctx->req,
            (void*)ctx,
            (void*)ctx->ch,
            node ? node : "<null>",
            service ? service : "<null>",
            (void*)g_loop,
            goc_loop_is_shutting_down());
}

static void dns_trace_submit_fail(goc_getaddrinfo_ctx_t* ctx, int rc)
{
    GOC_DBG("[DNS_TRACE] submit-fail req=%p ctx=%p ch=%p rc=%d loop=%p shutdown=%d\n",
            (void*)&ctx->req,
            (void*)ctx,
            (void*)ctx->ch,
            rc,
            (void*)g_loop,
            goc_loop_is_shutting_down());
}

static void dns_trace_callback(goc_getaddrinfo_ctx_t* ctx, int status)
{
    GOC_DBG("[DNS_TRACE] cb submit=%ld req=%p ctx=%p ch=%p status=%d loop=%p shutdown=%d\n",
            ctx->submit_seq,
            (void*)&ctx->req,
            (void*)ctx,
            (void*)ctx->ch,
            status,
            ctx->req.loop ? (void*)ctx->req.loop : (void*)g_loop,
            goc_loop_is_shutting_down());
}

static void on_getaddrinfo(uv_getaddrinfo_t* req, int status,
                           struct addrinfo* res)
{
    goc_getaddrinfo_ctx_t* ctx = (goc_getaddrinfo_ctx_t*)req;
    dns_trace_callback(ctx, status);
    goc_io_getaddrinfo_t*  r   = (goc_io_getaddrinfo_t*)goc_malloc(
                                     sizeof(goc_io_getaddrinfo_t));
    r->ok  = (status == 0) ? GOC_IO_OK : GOC_IO_ERR;
    r->res = res;
    goc_put_cb(ctx->ch, r, goc_close_cb, ctx->ch);
    free(ctx);
}

goc_chan* goc_io_getaddrinfo(const char* node, const char* service,
                                const struct addrinfo* hints)
{
    goc_chan*              ch  = goc_chan_make(1);
    if (goc_loop_is_shutting_down()) {
        goc_io_getaddrinfo_t* r = (goc_io_getaddrinfo_t*)goc_malloc(
                                   sizeof(goc_io_getaddrinfo_t));
        r->ok  = GOC_IO_ERR;
        r->res = NULL;
        goc_put_cb(ch, r, goc_close_cb, ch);
        return ch;
    }
    goc_getaddrinfo_ctx_t* ctx = (goc_getaddrinfo_ctx_t*)malloc(
                                     sizeof(goc_getaddrinfo_ctx_t));
    if (!ctx) {
        goc_io_getaddrinfo_t* r = (goc_io_getaddrinfo_t*)goc_malloc(
                                   sizeof(goc_io_getaddrinfo_t));
        r->ok  = GOC_IO_ERR;
        r->res = NULL;
        goc_put_cb(ch, r, goc_close_cb, ch);
        return ch;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->ch = ch;
    /* Trace submit before calling into libuv to avoid a race where callback
     * runs and frees ctx before post-call tracing executes on another thread. */
    dns_trace_submit(ctx, node, service);
    int rc = uv_getaddrinfo(goc_worker_or_default_loop(), &ctx->req, on_getaddrinfo,
                            node, service, hints);
    if (rc < 0) {
        dns_trace_submit_fail(ctx, rc);
        free(ctx);
        goc_io_getaddrinfo_t* r = (goc_io_getaddrinfo_t*)goc_malloc(
                                   sizeof(goc_io_getaddrinfo_t));
        r->ok  = GOC_IO_ERR;
        r->res = NULL;
        goc_put_cb(ch, r, goc_close_cb, ch);
        return ch;
    }
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
    goc_put_cb(ctx->ch, r, goc_close_cb, ctx->ch);
    free(ctx);
}

goc_chan* goc_io_getnameinfo(const struct sockaddr* addr, int flags)
{
    goc_chan*              ch  = goc_chan_make(1);
    goc_getnameinfo_ctx_t* ctx = (goc_getnameinfo_ctx_t*)malloc(
                                     sizeof(goc_getnameinfo_ctx_t));
    if (!ctx) {
        goc_io_getnameinfo_t* r = (goc_io_getnameinfo_t*)goc_malloc(
                                   sizeof(goc_io_getnameinfo_t));
        r->ok         = GOC_IO_ERR;
        r->hostname[0] = '\0';
        r->service[0]  = '\0';
        goc_put_cb(ch, r, goc_close_cb, ch);
        return ch;
    }
    ctx->ch = ch;
    int rc = uv_getnameinfo(goc_worker_or_default_loop(), &ctx->req, on_getnameinfo, addr, flags);
    if (rc < 0) {
        free(ctx);
        goc_io_getnameinfo_t* r = (goc_io_getnameinfo_t*)goc_malloc(
                                   sizeof(goc_io_getnameinfo_t));
        r->ok         = GOC_IO_ERR;
        r->hostname[0] = '\0';
        r->service[0]  = '\0';
        goc_put_cb(ch, r, goc_close_cb, ch);
        return ch;
    }
    return ch;
}


/* =========================================================================
 * 1. Stream I/O  (TCP, Pipes, TTY)
 *
 * Stream handle operations are NOT thread-safe.  They are dispatched to the
 * event loop thread via post_on_loop() (MPSC callback queue — no per-call
 * uv_async_t).
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

#define GOC_STREAM_CTX_MAGIC 0x53545243u  /* 'STRC' */
#define GOC_READ_START_PENDING_MAGIC 0x50524450u  /* 'PRDP' */
#define GOC_SERVER_CTX_MAGIC 0x53565243u      /* 'SVRC' */

typedef struct {
    uint32_t   magic;
    goc_chan*  ch;
    void*      saved_handle_data;
} goc_stream_ctx_t;

typedef struct {
    uint32_t   magic;
    goc_chan*  ch;
    int        is_tcp; /* 1=tcp 0=pipe */
} goc_server_ctx_t;

static uv_mutex_t   g_handle_registry_lock;
static goc_io_handle_t** g_handle_registry = NULL;
static size_t g_handle_registry_len = 0;
static size_t g_handle_registry_cap = 0;

static void goc_io_handle_registry_init(void)
{
    uv_mutex_init(&g_handle_registry_lock);
}

static void goc_io_handle_registry_shutdown(void)
{
    if (g_handle_registry) {
        free(g_handle_registry);
        g_handle_registry = NULL;
    }
    g_handle_registry_len = 0;
    g_handle_registry_cap = 0;
    uv_mutex_destroy(&g_handle_registry_lock);
}

static void goc_io_handle_registry_add(goc_io_handle_t* h)
{
    uv_mutex_lock(&g_handle_registry_lock);
    if (g_handle_registry_len == g_handle_registry_cap) {
        size_t new_cap = g_handle_registry_cap ? g_handle_registry_cap * 2 : 16;
        goc_io_handle_t** grown = realloc(g_handle_registry,
                                      new_cap * sizeof(goc_io_handle_t*));
        assert(grown != NULL);
        g_handle_registry = grown;
        g_handle_registry_cap = new_cap;
    }
    g_handle_registry[g_handle_registry_len++] = h;
    uv_mutex_unlock(&g_handle_registry_lock);
}

static goc_io_handle_t* goc_io_handle_registry_find(uv_handle_t* uv)
{
    if (!uv) {
        return NULL;
    }

    uv_mutex_lock(&g_handle_registry_lock);
    goc_io_handle_t* result = NULL;
    for (size_t i = 0; i < g_handle_registry_len; i++) {
        if (g_handle_registry[i]->uv == uv) {
            result = g_handle_registry[i];
            break;
        }
    }
    if (result) {
        GOC_DBG("goc_io_handle_registry_find: handle=%p recovered=%p registry_len=%zu\n",
                (void*)uv, (void*)result, g_handle_registry_len);
    } else {
        GOC_DBG("goc_io_handle_registry_find: handle=%p recovered=NULL registry_len=%zu\n",
                (void*)uv, g_handle_registry_len);
    }
    uv_mutex_unlock(&g_handle_registry_lock);
    return result;
}

static goc_io_handle_t* goc_io_handle_registry_remove(uv_handle_t* uv)
{
    if (!uv) {
        return NULL;
    }

    uv_mutex_lock(&g_handle_registry_lock);
    goc_io_handle_t* result = NULL;
    for (size_t i = 0; i < g_handle_registry_len; i++) {
        if (g_handle_registry[i]->uv == uv) {
            result = g_handle_registry[i];
            g_handle_registry_len--;
            if (i < g_handle_registry_len) {
                g_handle_registry[i] = g_handle_registry[g_handle_registry_len];
            }
            break;
        }
    }
    if (result) {
        GOC_DBG("goc_io_handle_registry_remove: handle=%p removed=%p remaining_len=%zu\n",
                (void*)uv, (void*)result, g_handle_registry_len);
    } else {
        GOC_DBG("goc_io_handle_registry_remove: handle=%p not found remaining_len=%zu\n",
                (void*)uv, g_handle_registry_len);
    }
    uv_mutex_unlock(&g_handle_registry_lock);
    return result;
}

static int uv_async_handle_owner_marker;
static void* const UV_ASYNC_HANDLE_OWNER =
    (void*)&uv_async_handle_owner_marker;

static bool is_goc_io_handle_wrapper(void* data)
{
    if (!data) return false;

    bool found = false;
    uv_mutex_lock(&g_handle_registry_lock);
    for (size_t i = 0; i < g_handle_registry_len; i++) {
        if (g_handle_registry[i] == data) {
            found = true;
            break;
        }
    }
    uv_mutex_unlock(&g_handle_registry_lock);
    return found;
}

static goc_io_handle_t* goc_io_handle_from_uv(uv_handle_t* handle)
{
    if (!handle) {
        return NULL;
    }
    if (is_goc_io_handle_wrapper(handle->data)) {
        return (goc_io_handle_t*)handle->data;
    }
    return goc_io_handle_registry_find(handle);
}

static bool goc_io_handle_claim_close(goc_io_handle_t* h)
{
    if (!h) {
        return false;
    }
    goc_io_handle_state_t expected = GOC_H_OPEN;
    bool claimed = atomic_compare_exchange_strong(&h->state,
                                                 &expected,
                                                 GOC_H_QUEUED);
    if (claimed) {
        GOC_DBG("goc_io_handle_claim_close: claimed close for wrapper=%p uv=%p state OPEN->QUEUED\n",
                (void*)h, (void*)h->uv);
    } else {
        GOC_DBG("goc_io_handle_claim_close: did not claim close for wrapper=%p uv=%p state=%d\n",
                (void*)h, (void*)h->uv, (int)h->state);
    }
    return claimed;
}

goc_io_handle_t* goc_io_handle_wrap(uv_handle_t* uv, void* owner)
{
    if (!uv) {
        return NULL;
    }

    goc_io_handle_t* h = (goc_io_handle_t*)goc_malloc(sizeof(goc_io_handle_t));
    if (!h) {
        ABORT("goc_io_handle_wrap: failed to allocate handle\n");
    }
    h->magic = 0x474f4348u; /* 'GOCH' */
    h->uv = uv;
    h->owner = owner;
    h->state = GOC_H_OPEN;
    goc_io_handle_registry_add(h);
    goc_worker_io_handle_opened();
    return h;
}

void goc_io_handle_set_owner(goc_io_handle_t* h, void* owner)
{
    if (!h) {
        return;
    }
    if (h->state != GOC_H_OPEN && h->state != GOC_H_CLOSING) {
        ABORT("goc_io_handle_set_owner: invalid state=%d owner=%p handle=%p\n",
              (int)h->state, owner, (void*)h->uv);
    }
    h->owner = owner;
}

static void goc_io_handle_close_internal(uv_handle_t* handle,
                                          uv_close_cb cb,
                                          goc_chan* ch,
                                          bool retry);

void goc_io_handle_request_close(goc_io_handle_t* h, uv_close_cb cb)
{
    if (!h) {
        return;
    }

    if (h->state != GOC_H_OPEN) {
        if (h->state == GOC_H_CLOSING || h->state == GOC_H_QUEUED) {
            GOC_DBG("DOUBLE CLOSE: %p\n", (void*)h->uv);
            return;
        }
        ABORT("goc_io_handle_request_close: invalid state=%d owner=%p handle=%p\n",
              (int)h->state, h->owner, (void*)h->uv);
    }

    goc_io_handle_close_internal(h->uv, cb, NULL, false);
}

bool goc_io_handle_is_open(goc_io_handle_t* h)
{
    return h && h->state == GOC_H_OPEN;
}

bool goc_io_handle_is_owned_by(goc_io_handle_t* h, void* owner)
{
    return h && (h->owner == owner || h->owner == UV_ASYNC_HANDLE_OWNER);
}

typedef struct goc_read_start_pending {
    uint32_t                        magic;
    uv_stream_t*                    stream;
    goc_chan*                       ch;
    int                             canceled;
    void*                           saved_handle_data;
    struct goc_read_start_pending*  next;
} goc_read_start_pending_t;

static goc_read_start_pending_t* g_pending_read_starts = NULL;
static uv_mutex_t               g_pending_read_start_lock;
static uv_mutex_t               g_deferred_handle_unreg_lock;
static uv_handle_t**            g_deferred_handle_unregs = NULL;
static size_t                   g_deferred_handle_unregs_len = 0;
static size_t                   g_deferred_handle_unregs_cap = 0;

static uv_mutex_t               g_deferred_wrapper_free_lock;
static goc_io_handle_t**           g_deferred_wrapper_frees = NULL;
static size_t                   g_deferred_wrapper_frees_len = 0;
static size_t                   g_deferred_wrapper_frees_cap = 0;

typedef struct {
    uv_handle_t* target;
    uv_close_cb  cb;
    goc_chan*    ch;
} goc_deferred_close_t;

static uv_mutex_t               g_deferred_close_lock;
static goc_deferred_close_t*    g_deferred_closes = NULL;
static size_t                   g_deferred_closes_len = 0;
static size_t                   g_deferred_closes_cap = 0;

static bool goc_deferred_close_contains(uv_handle_t* handle)
{
    bool found = false;
    uv_mutex_lock(&g_deferred_close_lock);
    for (size_t i = 0; i < g_deferred_closes_len; i++) {
        if (g_deferred_closes[i].target == handle) {
            found = true;
            break;
        }
    }
    uv_mutex_unlock(&g_deferred_close_lock);
    return found;
}

void goc_deferred_handle_unreg_init(void)
{
    uv_mutex_init(&g_deferred_handle_unreg_lock);
    uv_mutex_init(&g_deferred_wrapper_free_lock);
    uv_mutex_init(&g_deferred_close_lock);
}

void goc_deferred_handle_unreg_shutdown(void)
{
    if (g_deferred_handle_unregs) {
        free(g_deferred_handle_unregs);
        g_deferred_handle_unregs = NULL;
    }
    g_deferred_handle_unregs_len = 0;
    g_deferred_handle_unregs_cap = 0;
    if (g_deferred_wrapper_frees) {
        free(g_deferred_wrapper_frees);
        g_deferred_wrapper_frees = NULL;
    }
    g_deferred_wrapper_frees_len = 0;
    g_deferred_wrapper_frees_cap = 0;
    if (g_deferred_closes) {
        free(g_deferred_closes);
        g_deferred_closes = NULL;
    }
    g_deferred_closes_len = 0;
    g_deferred_closes_cap = 0;
    uv_mutex_destroy(&g_deferred_handle_unreg_lock);
    uv_mutex_destroy(&g_deferred_wrapper_free_lock);
    uv_mutex_destroy(&g_deferred_close_lock);
}

void goc_deferred_handle_unreg_add(uv_handle_t* handle)
{
    if (!handle) return;
    uv_mutex_lock(&g_deferred_handle_unreg_lock);
    if (g_deferred_handle_unregs_len == g_deferred_handle_unregs_cap) {
        size_t new_cap = g_deferred_handle_unregs_cap ? g_deferred_handle_unregs_cap * 2 : 16;
        uv_handle_t** grown = realloc(g_deferred_handle_unregs,
                                      new_cap * sizeof(uv_handle_t*));
        assert(grown != NULL);
        g_deferred_handle_unregs = grown;
        g_deferred_handle_unregs_cap = new_cap;
    }
    g_deferred_handle_unregs[g_deferred_handle_unregs_len++] = handle;
    GOC_DBG("goc_deferred_handle_unreg_add: queued handle=%p len=%zu\n",
            (void*)handle, g_deferred_handle_unregs_len);
    uv_mutex_unlock(&g_deferred_handle_unreg_lock);
}

void goc_deferred_wrapper_free_add(goc_io_handle_t* handle)
{
    if (!handle) return;
    uv_mutex_lock(&g_deferred_wrapper_free_lock);
    if (g_deferred_wrapper_frees_len == g_deferred_wrapper_frees_cap) {
        size_t new_cap = g_deferred_wrapper_frees_cap ? g_deferred_wrapper_frees_cap * 2 : 16;
        goc_io_handle_t** grown = realloc(g_deferred_wrapper_frees,
                                      new_cap * sizeof(goc_io_handle_t*));
        assert(grown != NULL);
        g_deferred_wrapper_frees = grown;
        g_deferred_wrapper_frees_cap = new_cap;
    }
    g_deferred_wrapper_frees[g_deferred_wrapper_frees_len++] = handle;
    GOC_DBG("goc_deferred_wrapper_free_add: queued wrapper=%p state=%d len=%zu\n",
            (void*)handle, (int)handle->state, g_deferred_wrapper_frees_len);
    uv_mutex_unlock(&g_deferred_wrapper_free_lock);
}

static void register_handle_close_cb(uv_handle_t* handle,
                                      uv_close_cb user_cb,
                                      goc_chan* ch);
static void goc_io_handle_close_internal(uv_handle_t* handle,
                                          uv_close_cb cb,
                                          goc_chan* ch,
                                          bool retry);

void goc_deferred_close_add(uv_handle_t* handle, uv_close_cb cb, goc_chan* ch)
{
    if (!handle) return;

    uv_mutex_lock(&g_deferred_close_lock);
    for (size_t i = 0; i < g_deferred_closes_len; i++) {
        if (g_deferred_closes[i].target == handle) {
            if (cb || ch) {
                if (g_deferred_closes_len == g_deferred_closes_cap) {
                    size_t new_cap = g_deferred_closes_cap ? g_deferred_closes_cap * 2 : 16;
                    goc_deferred_close_t* grown = realloc(g_deferred_closes,
                                                         new_cap * sizeof(goc_deferred_close_t));
                    assert(grown != NULL);
                    g_deferred_closes = grown;
                    g_deferred_closes_cap = new_cap;
                }
                g_deferred_closes[g_deferred_closes_len].target = handle;
                g_deferred_closes[g_deferred_closes_len].cb = cb;
                g_deferred_closes[g_deferred_closes_len].ch = ch;
                g_deferred_closes_len++;
            }
            GOC_DBG("goc_deferred_close_add: handle already queued target=%p cb=%p ch=%p\n",
                    (void*)handle, (void*)cb, (void*)ch);
            uv_mutex_unlock(&g_deferred_close_lock);
            return;
        }
    }

    if (g_deferred_closes_len == g_deferred_closes_cap) {
        size_t new_cap = g_deferred_closes_cap ? g_deferred_closes_cap * 2 : 16;
        goc_deferred_close_t* grown = realloc(g_deferred_closes,
                                             new_cap * sizeof(goc_deferred_close_t));
        assert(grown != NULL);
        g_deferred_closes = grown;
        g_deferred_closes_cap = new_cap;
    }
    g_deferred_closes[g_deferred_closes_len].target = handle;
    g_deferred_closes[g_deferred_closes_len].cb = cb;
    g_deferred_closes[g_deferred_closes_len].ch = ch;
    g_deferred_closes_len++;
    GOC_DBG("goc_deferred_close_add: queued target=%p cb=%p ch=%p deferred_len=%zu thread=%llu\n",
            (void*)handle, (void*)cb, (void*)ch, g_deferred_closes_len, goc_uv_thread_id());
    uv_mutex_unlock(&g_deferred_close_lock);
}

void goc_deferred_close_flush(void)
{
    goc_deferred_close_t* items = NULL;
    size_t len = 0;

    uv_mutex_lock(&g_deferred_close_lock);
    if (g_deferred_closes_len > 0) {
        items = g_deferred_closes;
        len = g_deferred_closes_len;
        g_deferred_closes = NULL;
        g_deferred_closes_len = 0;
        g_deferred_closes_cap = 0;
    }
    uv_mutex_unlock(&g_deferred_close_lock);

    if (items) {
        GOC_DBG("goc_deferred_close_flush: flushing %zu deferred close(s)\n", len);
        for (size_t i = 0; i < len; i++) {
            uv_handle_t* target = items[i].target;
            GOC_DBG("goc_deferred_close_flush: target=%p type=%s active=%d closing=%d has_ref=%d loop=%p loop_alive=%d cb=%p ch=%p\n",
                    (void*)target,
                    target ? uv_handle_type_name(uv_handle_get_type(target)) : "<null>",
                    target ? uv_is_active(target) : -1,
                    target ? uv_is_closing(target) : -1,
                    target ? uv_has_ref(target) : -1,
                    target ? (void*)target->loop : NULL,
                    target && target->loop ? uv_loop_alive(target->loop) : -1,
                    (void*)items[i].cb,
                    (void*)items[i].ch);
            goc_io_handle_close_internal(items[i].target,
                                          items[i].cb,
                                          items[i].ch,
                                          true);
        }
        free(items);
    } else {
        GOC_DBG("goc_deferred_close_flush: no deferred closes to flush\n");
    }
}

size_t goc_deferred_close_len(void)
{
    size_t len = 0;
    uv_mutex_lock(&g_deferred_close_lock);
    len = g_deferred_closes_len;
    uv_mutex_unlock(&g_deferred_close_lock);
    return len;
}

void goc_deferred_handle_unreg_flush(void)
{
    uv_handle_t** items = NULL;
    size_t len = 0;

    uv_mutex_lock(&g_deferred_handle_unreg_lock);
    if (g_deferred_handle_unregs_len > 0) {
        items = g_deferred_handle_unregs;
        len = g_deferred_handle_unregs_len;
        g_deferred_handle_unregs = NULL;
        g_deferred_handle_unregs_len = 0;
        g_deferred_handle_unregs_cap = 0;
    }
    uv_mutex_unlock(&g_deferred_handle_unreg_lock);

    if (items) {
        GOC_DBG("goc_deferred_handle_unreg_flush: unregistering %zu handle(s)\n", len);
        for (size_t i = 0; i < len; i++) {
            uv_handle_t* h = items[i];
            GOC_DBG("goc_deferred_handle_unreg_flush: unregister handle=%p type=%s active=%d closing=%d has_ref=%d loop=%p loop_alive=%d data=%p\n",
                    (void*)h,
                    h ? uv_handle_type_name(uv_handle_get_type(h)) : "<null>",
                    h ? uv_is_active(h) : -1,
                    h ? uv_is_closing(h) : -1,
                    h ? uv_has_ref(h) : -1,
                    h ? (void*)h->loop : NULL,
                    h && h->loop ? uv_loop_alive(h->loop) : -1,
                    h ? h->data : NULL);
            gc_handle_unregister(items[i]);
        }
        free(items);
    } else {
        GOC_DBG("goc_deferred_handle_unreg_flush: no handles to unregister\n");
    }

    uv_handle_t* dummy = NULL; (void)dummy;
}

size_t goc_deferred_handle_unreg_len(void)
{
    size_t len = 0;
    uv_mutex_lock(&g_deferred_handle_unreg_lock);
    len = g_deferred_handle_unregs_len;
    uv_mutex_unlock(&g_deferred_handle_unreg_lock);
    return len;
}

void goc_io_init(void)
{
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif
    uv_mutex_init(&g_pending_read_start_lock);
    goc_io_handle_registry_init();
    goc_deferred_handle_unreg_init();
}

void goc_io_shutdown(void)
{
    while (g_pending_read_starts) {
        goc_read_start_pending_t* next = g_pending_read_starts->next;
        free(g_pending_read_starts);
        g_pending_read_starts = next;
    }
    uv_mutex_destroy(&g_pending_read_start_lock);
    goc_io_handle_registry_shutdown();
    goc_deferred_handle_unreg_shutdown();
}

static void pending_read_start_add(goc_read_start_pending_t* p)
{
    uv_mutex_lock(&g_pending_read_start_lock);
    p->next = g_pending_read_starts;
    g_pending_read_starts = p;
    GOC_DBG("pending_read_start_add: added pending=%p stream=%p ch=%p\n",
            (void*)p, (void*)p->stream, (void*)p->ch);
    uv_mutex_unlock(&g_pending_read_start_lock);
}

static int pending_read_start_remove(goc_read_start_pending_t* p)
{
    uv_mutex_lock(&g_pending_read_start_lock);
    goc_read_start_pending_t** cur = &g_pending_read_starts;
    while (*cur) {
        if (*cur == p) {
            *cur = p->next;
            GOC_DBG("pending_read_start_remove: removed pending=%p stream=%p ch=%p\n",
                    (void*)p, (void*)p->stream, (void*)p->ch);
            uv_mutex_unlock(&g_pending_read_start_lock);
            return 1;
        }
        cur = &(*cur)->next;
    }
    GOC_DBG("pending_read_start_remove: not found pending=%p stream=%p ch=%p\n",
            (void*)p, (void*)p->stream, (void*)p->ch);
    uv_mutex_unlock(&g_pending_read_start_lock);
    return 0;
}

static goc_read_start_pending_t* pending_read_start_take_for_stream(
        uv_stream_t* stream)
{
    GOC_DBG("pending_read_start_take_for_stream: scan stream=%p\n",
            (void*)stream);
    uv_mutex_lock(&g_pending_read_start_lock);
    goc_read_start_pending_t** cur = &g_pending_read_starts;
    goc_read_start_pending_t* head = NULL;
    goc_read_start_pending_t* tail = NULL;
    while (*cur) {
        if ((*cur)->stream == stream) {
            goc_read_start_pending_t* taken = *cur;
            *cur = taken->next;
            taken->next = NULL;
            if (tail)
                tail->next = taken;
            else
                head = taken;
            tail = taken;
            GOC_DBG("pending_read_start_take_for_stream: took pending=%p stream=%p ch=%p\n",
                    (void*)taken, (void*)taken->stream, (void*)taken->ch);
            continue;
        }
        cur = &(*cur)->next;
    }
    uv_mutex_unlock(&g_pending_read_start_lock);
    GOC_DBG("pending_read_start_take_for_stream: result head=%p stream=%p\n",
            (void*)head, (void*)stream);
    return head;
}

static int is_pending_read_start_marker(void* data, uv_stream_t* stream)
{
    if (!data || !GC_is_heap_ptr(data)) {
        return 0;
    }
    goc_read_start_pending_t* pending = (goc_read_start_pending_t*)data;
    return pending->magic == GOC_READ_START_PENDING_MAGIC &&
           pending->stream == stream;
}

static int is_server_ctx(void* data)
{
    if (!data || !GC_is_heap_ptr(data)) {
        return 0;
    }
    goc_server_ctx_t* ctx = (goc_server_ctx_t*)data;
    return ctx->magic == GOC_SERVER_CTX_MAGIC;
}

static int is_goc_stream_ctx(void* data)
{
    if (!data || !GC_is_heap_ptr(data)) {
        return 0;
    }
    goc_stream_ctx_t* ctx = (goc_stream_ctx_t*)data;
    return ctx->magic == GOC_STREAM_CTX_MAGIC;
}

static void on_read_cb(uv_stream_t* stream, ssize_t nread,
                       const uv_buf_t* buf)
{
    if (!is_goc_stream_ctx(stream->data)) {
        goc_stream_ctx_t* ctx = (goc_stream_ctx_t*)stream->data;
        GOC_DBG("on_read_cb: stream=%p invalid/cleared ctx=%p stream->data=%p dropping buf\n",
                (void*)stream, (void*)ctx, stream->data);
        free(buf->base);
        return;
    }
    goc_stream_ctx_t* ctx = (goc_stream_ctx_t*)stream->data;
    GOC_DBG("on_read_cb: stream=%p nread=%zd stream->data=%p ctx=%p ch=%p\n",
            (void*)stream, nread, stream->data, (void*)ctx, (void*)ctx->ch);

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
        GOC_DBG("on_read_cb: delivering data nread=%zd stream=%p ch=%p\n",
                nread, (void*)stream, (void*)ctx->ch);
        goc_put_cb(ctx->ch, res, NULL, NULL);
        GOC_DBG("on_read_cb: delivered data to ch=%p stream=%p\n",
                (void*)ctx->ch, (void*)stream);
        return;
    }

    /* nread < 0: EOF or error.  Free the unused malloc'd buffer, deliver
     * the final (error) result.  Do NOT close the channel or clear
     * stream->data here: on_read_stop_dispatch is responsible for calling
     * goc_close and clearing stream->data.  Closing here races with a
     * pending goc_put_cb for the last data chunk (both queued in the same
     * libuv tick), causing loop_process_pending_put to see ch->closed==1
     * and drop the data before the reader ever sees it. */
    GOC_DBG("on_read_cb: nread<0 (EOF/error=%zd) stream=%p ctx=%p ch=%p saved_handle_data=%p, posting EOF result via goc_close(caller=eof_path)\n",
            nread, (void*)stream, (void*)ctx, (void*)ctx->ch, ctx->saved_handle_data);
    free(buf->base);
    res->buf = NULL;
    goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);  /* res freed by consumer */
    GOC_DBG("on_read_cb: posted goc_close to ch=%p stream=%p, restoring stream->data from ctx=%p\n",
            (void*)ctx->ch, (void*)stream, (void*)ctx);
    stream->data = ctx->saved_handle_data;
}

typedef struct {
    uv_stream_t*                   stream;
    goc_chan*                      ch;
    goc_read_start_pending_t*      pending;
} goc_read_start_dispatch_t;

static void on_read_start_dispatch(void* arg)
{
    goc_read_start_dispatch_t* d = (goc_read_start_dispatch_t*)arg;
    GOC_DBG(
            "on_read_start_dispatch: ENTERED d=%p stream=%p loop=%p ch=%p pending=%p\n",
            (void*)d, (void*)d->stream, d->stream ? (void*)d->stream->loop : NULL,
            (void*)d->ch, (void*)d->pending);
    if (!pending_read_start_remove(d->pending)) {
        GOC_DBG(
                "on_read_start_dispatch: pending read start already removed/canceled before dispatch stream=%p ch=%p pending=%p\n",
                (void*)d->stream, (void*)d->ch, (void*)d->pending);
        free(d);
        return;
    }
    GOC_DBG("on_read_start_dispatch: pending read start remove succeeded stream=%p ch=%p pending=%p\n",
            (void*)d->stream, (void*)d->ch, (void*)d->pending);
    if (d->stream && d->stream->data) {
        if (is_goc_io_handle_wrapper(d->stream->data) || is_server_ctx(d->stream->data)) {
            d->pending->saved_handle_data = d->stream->data;
        } else {
            GOC_DBG("on_read_start_dispatch: stream->data already non-NULL before assign stream=%p data=%p ch=%p\n",
                    (void*)d->stream, d->stream->data, (void*)d->ch);
            goc_io_read_t* res = (goc_io_read_t*)goc_malloc(sizeof(goc_io_read_t));
            res->nread = UV_EBUSY;
            res->buf   = NULL;
            goc_put_cb(d->ch, res, goc_close_cb, d->ch);
            free(d);
            return;
        }
    }

    if (d->stream) {
        d->stream->data = d->pending;
        GOC_DBG("on_read_start_dispatch: stream->data set to pending marker=%p stream=%p ch=%p\n",
                (void*)d->pending, (void*)d->stream, (void*)d->ch);
    }

    if (d->pending->canceled) {
        GOC_DBG("on_read_start_dispatch: pending read start was canceled in-flight stream=%p ch=%p pending=%p\n",
                (void*)d->stream, (void*)d->ch, (void*)d->pending);
        goc_io_read_t* res = (goc_io_read_t*)goc_malloc(sizeof(goc_io_read_t));
        res->nread = UV_ECANCELED;
        res->buf   = NULL;
        goc_put_cb(d->ch, res, goc_close_cb, d->ch);
        if (d->stream && d->stream->data == d->pending)
            d->stream->data = d->pending->saved_handle_data;
        free(d);
        return;
    }

    if (!d->stream ||
        goc_loop_is_shutting_down() ||
        uv_is_closing((uv_handle_t*)d->stream)) {
        GOC_DBG(
                "on_read_start_dispatch: preflight-reject stream=%p loop=%p closing=%d shutdown=%d data=%p\n",
                (void*)d->stream,
                d->stream ? (void*)d->stream->loop : NULL,
                d->stream ? uv_is_closing((uv_handle_t*)d->stream) : -1,
                goc_loop_is_shutting_down(),
                d->stream ? d->stream->data : NULL);
        goc_io_read_t* res = (goc_io_read_t*)goc_malloc(sizeof(goc_io_read_t));
        res->nread = UV_ECANCELED;
        res->buf   = NULL;
        goc_put_cb(d->ch, res, goc_close_cb, d->ch);
        if (d->stream && d->stream->data == d->pending)
            d->stream->data = d->pending->saved_handle_data;
        GOC_DBG(
                "on_read_start_dispatch: posted preflight ECANCELED to ch=%p stream=%p\n",
                (void*)d->ch,
                (void*)d->stream);
        free(d);
        return;
    }

    if (d->stream->data != d->pending) {
        GOC_DBG("on_read_start_dispatch: stream->data already non-NULL before assign stream=%p data=%p new_ctx=%p ch=%p\n",
                (void*)d->stream, d->stream->data, (void*)NULL, (void*)d->ch);
        goc_io_read_t* res = (goc_io_read_t*)goc_malloc(sizeof(goc_io_read_t));
        res->nread = UV_EBUSY;
        res->buf   = NULL;
        goc_put_cb(d->ch, res, goc_close_cb, d->ch);
        free(d);
        return;
    }

    if (d->pending->canceled) {
        GOC_DBG("on_read_start_dispatch: pending read start was canceled before uv_read_start stream=%p ch=%p pending=%p\n",
                (void*)d->stream, (void*)d->ch, (void*)d->pending);
        goc_io_read_t* res = (goc_io_read_t*)goc_malloc(sizeof(goc_io_read_t));
        res->nread = UV_ECANCELED;
        res->buf   = NULL;
        goc_put_cb(d->ch, res, goc_close_cb, d->ch);
        d->stream->data = NULL;
        free(d);
        return;
    }

    goc_stream_ctx_t* ctx = (goc_stream_ctx_t*)goc_malloc(
                                         sizeof(goc_stream_ctx_t));
    ctx->magic = GOC_STREAM_CTX_MAGIC;
    ctx->ch    = d->ch;
    ctx->saved_handle_data = d->pending->saved_handle_data;
    d->stream->data = ctx;
    GOC_DBG("on_read_start_dispatch: stream->data set to read ctx=%p stream=%p ch=%p\n",
            (void*)ctx, (void*)d->stream, (void*)ctx->ch);
    if (d->pending->canceled) {
        GOC_DBG("on_read_start_dispatch: pending read start canceled after ctx allocation stream=%p ch=%p pending=%p\n",
                (void*)d->stream, (void*)d->ch, (void*)d->pending);
        goc_io_read_t* res = (goc_io_read_t*)goc_malloc(sizeof(goc_io_read_t));
        res->nread = UV_ECANCELED;
        res->buf   = NULL;
        goc_put_cb(d->ch, res, goc_close_cb, d->ch);
        if (d->stream && d->stream->data == ctx)
            d->stream->data = ctx->saved_handle_data;
        free(d);
        return;
    }
    GOC_DBG(
            "on_read_start_dispatch: ENTER stream=%p loop=%p ch=%p current_worker=%d\n",
            (void*)d->stream, (void*)d->stream->loop, (void*)ctx->ch,
            goc_current_worker_id());
    GOC_DBG("on_read_start_dispatch: set stream->data=%p ctx->ch=%p\n",
            (void*)ctx, (void*)ctx->ch);

    int rc = uv_read_start(d->stream, goc_alloc_cb, on_read_cb);
    GOC_DBG(
            "on_read_start_dispatch: uv_read_start rc=%d stream=%p loop=%p ch=%p\n",
            rc, (void*)d->stream, (void*)d->stream->loop, (void*)ctx->ch);
    GOC_DBG("on_read_start_dispatch: uv_read_start rc=%d\n", rc);
    if (rc < 0) {
        /* Failed to start: deliver error and close channel. */
        goc_io_read_t* res = (goc_io_read_t*)goc_malloc(sizeof(goc_io_read_t));
        res->nread = rc;
        res->buf   = NULL;
        goc_put_cb(d->ch, res, goc_close_cb, d->ch);
        if (d->stream && d->stream->data == ctx)
            d->stream->data = ctx->saved_handle_data;
        /* ctx is GC-managed via goc_malloc; do not free it manually. */
    }

    free(d);
}

goc_chan* goc_io_read_start(uv_stream_t* stream)
{
    int worker_id = goc_current_worker_id();
    GOC_DBG(
            "goc_io_read_start: current_worker=%d stream=%p loop=%p stream->data=%p\n",
            worker_id, (void*)stream, (void*)stream->loop, (void*)stream->data);
    GOC_DBG("goc_io_read_start: before goc_chan_make stream=%p\n", (void*)stream);
    goc_chan*                   ch = goc_chan_make(16);
    goc_chan_set_debug_tag(ch, "goc_io_read_start_ch");
    GOC_DBG(
            "goc_io_read_start: stream=%p new ch=%p (buf=16) stream->data=%p loop=%p\n",
            (void*)stream, (void*)ch, stream->data, (void*)stream->loop);
    goc_read_start_dispatch_t*  d  = (goc_read_start_dispatch_t*)malloc(
                                         sizeof(goc_read_start_dispatch_t));
    goc_read_start_pending_t*   p  = (goc_read_start_pending_t*)goc_malloc(
                                         sizeof(goc_read_start_pending_t));
    p->magic            = GOC_READ_START_PENDING_MAGIC;
    p->stream           = stream;
    p->ch               = ch;
    p->canceled         = 0;
    p->saved_handle_data = NULL;
    GOC_DBG("goc_io_read_start: adding pending read-start stream=%p ch=%p pending=%p\n",
            (void*)stream, (void*)ch, (void*)p);
    pending_read_start_add(p);
    d->stream  = stream;
    d->ch      = ch;
    d->pending = p;
    dispatch_on_handle_loop(stream->loop, on_read_start_dispatch, d);
    GOC_DBG(
            "goc_io_read_start: dispatched read-start for stream=%p ch=%p loop=%p pending=%p\n",
            (void*)stream, (void*)ch, (void*)stream->loop, (void*)p);
    GOC_DBG(
            "goc_io_read_start: dispatch complete stream=%p ch=%p loop=%p\n",
            (void*)stream, (void*)ch, (void*)stream->loop);
    return ch;
}

/* -------------------------------------------------------------------------
 * goc_io_read_stop
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_stream_t* stream;
    goc_chan*    ch;
} goc_read_stop_dispatch_t;

static void on_read_stop_dispatch(void* arg)
{
    goc_read_stop_dispatch_t* d = (goc_read_stop_dispatch_t*)arg;
    GOC_DBG(
            "on_read_stop_dispatch: ENTERED d=%p stream=%p loop=%p data=%p\n",
            (void*)d, (void*)d->stream,
            d->stream ? (void*)d->stream->loop : NULL,
            d->stream ? d->stream->data : NULL);

    if (is_goc_stream_ctx(d->stream->data)) {
        goc_stream_ctx_t* ctx = (goc_stream_ctx_t*)d->stream->data;
        if (!uv_is_closing((uv_handle_t*)d->stream)) {
            int stop_rc = uv_read_stop(d->stream);
            GOC_DBG(
                    "on_read_stop_dispatch: uv_read_stop returned %d stream=%p data=%p\n",
                    stop_rc, (void*)d->stream,
                    d->stream ? d->stream->data : NULL);
        } else {
            GOC_DBG(
                    "on_read_stop_dispatch: stream=%p already closing, skipping uv_read_stop\n",
                    (void*)d->stream);
        }
        GOC_DBG("on_read_stop_dispatch: stream->data non-NULL, closing ch=%p caller=read_stop ctx=%p stream=%p\n",
                (void*)ctx->ch, (void*)ctx, (void*)d->stream);
        /* Wake taker with EOF before close. The read channel is closed by
         * goc_close when the EOF result is delivered. */
        goc_io_read_t* eof = (goc_io_read_t*)goc_malloc(sizeof(goc_io_read_t));
        eof->nread = UV_EOF;
        eof->buf   = NULL;
        goc_put_cb(ctx->ch, eof, goc_close_cb, ctx->ch);
        GOC_DBG("on_read_stop_dispatch: posted EOF result with goc_close to ch=%p stream=%p reason=read_stop\n",
                (void*)ctx->ch, (void*)d->stream);
        d->stream->data = ctx->saved_handle_data;
        GOC_DBG("on_read_stop_dispatch: restored stream->data for stream=%p after read-stop ch=%p\n",
                (void*)d->stream, (void*)ctx->ch);
        free(d);
        return;
    }
    if (is_pending_read_start_marker(d->stream->data, d->stream)) {
        goc_read_start_pending_t* pending = (goc_read_start_pending_t*)d->stream->data;
        pending->canceled = 1;
        d->stream->data = pending->saved_handle_data;
        GOC_DBG("on_read_stop_dispatch: canceled in-flight pending read start stream=%p ch=%p pending=%p saved_data=%p\n",
                (void*)d->stream, (void*)pending->ch, (void*)pending, d->stream->data);
    } else if (d->stream->data) {
        GOC_DBG(
                "on_read_stop_dispatch: stream->data present but not a read context, preserving non-read data stream=%p data=%p\n",
                (void*)d->stream, d->stream->data);
    } else if (d->ch) {
        GOC_DBG(
                "on_read_stop_dispatch: no stream->data, delivering EOF to stored ch=%p stream=%p\n",
                (void*)d->ch, (void*)d->stream);
        goc_io_read_t* eof = (goc_io_read_t*)goc_malloc(sizeof(goc_io_read_t));
        eof->nread = UV_EOF;
        eof->buf   = NULL;
        goc_put_cb(d->ch, eof, goc_close_cb, d->ch);
        GOC_DBG(
                "on_read_stop_dispatch: posted EOF result with goc_close to stored ch=%p stream=%p reason=read_stop_fallback\n",
                (void*)d->ch, (void*)d->stream);
    } else {
        GOC_DBG("on_read_stop_dispatch: no stream->data, skipping uv_read_stop for stream=%p\n",
                (void*)d->stream);
    }

    GOC_DBG(
            "on_read_stop_dispatch: stream->data is %p — possible pending read-start cancellation caller=read_stop stream=%p loop=%p\n",
            d->stream ? d->stream->data : NULL,
            (void*)d->stream,
            d->stream ? (void*)d->stream->loop : NULL);
    goc_read_start_pending_t* pending = pending_read_start_take_for_stream(d->stream);
    if (pending) {
        while (pending) {
            goc_read_start_pending_t* next = pending->next;
            GOC_DBG(
                    "on_read_stop_dispatch: canceled pending read start stream=%p ch=%p pending=%p\n",
                    (void*)d->stream, (void*)pending->ch, (void*)pending);
            goc_io_read_t* eof = (goc_io_read_t*)goc_malloc(sizeof(goc_io_read_t));
            eof->nread = UV_ECANCELED;
            eof->buf   = NULL;
            goc_put_cb(pending->ch, eof, goc_close_cb, pending->ch);
            GOC_DBG(
                    "on_read_stop_dispatch: posted ECANCELED with goc_close to pending ch=%p stream=%p\n",
                    (void*)pending->ch, (void*)d->stream);
            pending = next;
        }
    } else {
        GOC_DBG(
                "on_read_stop_dispatch: no pending read start for stream=%p caller=read_stop\n",
                (void*)d->stream);
    }
    GOC_DBG(
            "on_read_stop_dispatch: completed read-stop dispatch for stream=%p\n",
            (void*)d->stream);
    free(d);
}

int goc_io_read_stop(uv_stream_t* stream)
{
    GOC_DBG("goc_io_read_stop: stream=%p stream->data=%p caller=fiber\n",
            (void*)stream, stream->data);
    goc_read_stop_dispatch_t* d = (goc_read_stop_dispatch_t*)malloc(
                                      sizeof(goc_read_stop_dispatch_t));
    d->stream = stream;
    d->ch = is_goc_stream_ctx(stream->data)
                ? ((goc_stream_ctx_t*)stream->data)->ch
                : NULL;
    GOC_DBG("goc_io_read_stop: posting loop task d=%p\n", (void*)d);
    dispatch_on_handle_loop(stream->loop, on_read_stop_dispatch, d);
    GOC_DBG("goc_io_read_stop: task posted\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * goc_io_write
 * ---------------------------------------------------------------------- */

typedef struct goc_write_ctx {
    uv_write_t  req;    /* MUST be first member */
    goc_chan*   ch;
    uv_buf_t*   bufs_copy;
    unsigned    nbufs;
    uv_stream_t* handle;
    int         canceled;
    struct goc_write_ctx* next;
} goc_write_ctx_t;

static _Thread_local goc_write_ctx_t* g_pending_write_ctxs = NULL;
static _Atomic long g_uv_active_reqs = 0;

static void free_owned_bufs(uv_buf_t* bufs, unsigned nbufs)
{
    if (!bufs)
        return;
    for (unsigned i = 0; i < nbufs; i++)
        free(bufs[i].base);
    free(bufs);
}

static void pending_write_add(goc_write_ctx_t* ctx)
{
    ctx->next = g_pending_write_ctxs;
    g_pending_write_ctxs = ctx;
}

static void pending_write_remove(goc_write_ctx_t* ctx)
{
    goc_write_ctx_t** cur = &g_pending_write_ctxs;
    while (*cur) {
        if (*cur == ctx) {
            *cur = ctx->next;
            return;
        }
        cur = &(*cur)->next;
    }
}

static void pending_write_cancel_for_handle(uv_stream_t* handle)
{
    GOC_DBG("pending_write_cancel_for_handle: handle=%p data=%p\n",
            (void*)handle, handle ? handle->data : NULL);
    size_t canceled_count = 0;
    goc_write_ctx_t** cur = &g_pending_write_ctxs;
    while (*cur) {
        goc_write_ctx_t* next = (*cur)->next;
        if ((*cur)->handle == handle) {
            goc_write_ctx_t* canceled = *cur;
            *cur = next;
            canceled_count++;
            GOC_DBG("pending_write_cancel_for_handle: canceling write ctx=%p ch=%p handle=%p\n",
                    (void*)canceled, (void*)canceled->ch, (void*)handle);
            if (canceled->ch) {
                goc_put_cb(canceled->ch, SCALAR(UV_ECANCELED), goc_close_cb, canceled->ch);
                canceled->ch = NULL;
            } else {
                GOC_DBG("pending_write_cancel_for_handle: write ctx=%p already had no channel ch=NULL\n",
                        (void*)canceled);
            }
            canceled->canceled = 1;
            GOC_DBG("pending_write_cancel_for_handle: canceled write ctx=%p handle=%p\n",
                    (void*)canceled, (void*)handle);
        } else {
            cur = &(*cur)->next;
        }
    }
    GOC_DBG("pending_write_cancel_for_handle: completed handle=%p canceled_count=%zu\n",
            (void*)handle, canceled_count);
}

static void on_write_cb(uv_write_t* req, int status)
{
    goc_write_ctx_t* ctx = (goc_write_ctx_t*)req;
    atomic_fetch_sub_explicit(&g_uv_active_reqs, 1, memory_order_acq_rel);
    GOC_DBG(
            "on_write_cb: req=%p status=%d ch=%p canceled=%d handle=%p active=%d closing=%d active_reqs=%ld\n",
            (void*)req,
            status,
            (void*)ctx->ch,
            ctx->canceled,
            (void*)ctx->handle,
            ctx->handle ? uv_is_active((uv_handle_t*)ctx->handle) : -1,
            ctx->handle ? uv_is_closing((uv_handle_t*)ctx->handle) : -1,
            atomic_load_explicit(&g_uv_active_reqs, memory_order_relaxed));
    if (ctx->canceled) {
        GOC_DBG("on_write_cb: write callback invoked for canceled ctx=%p handle=%p ch=%p\n",
                (void*)ctx, (void*)ctx->handle, (void*)ctx->ch);
    }
    pending_write_remove(ctx);
    if (!ctx->canceled) {
        goc_put_cb(ctx->ch, SCALAR(status), goc_close_cb, ctx->ch);
    }
    free_owned_bufs(ctx->bufs_copy, ctx->nbufs);
    free(ctx);
}

typedef struct {
    uv_stream_t*    handle;
    const uv_buf_t* bufs;
    unsigned int    nbufs;
    goc_chan*       ch;
} goc_write_dispatch_t;

static void on_write_dispatch(void* arg)
{
    goc_write_dispatch_t* d   = (goc_write_dispatch_t*)arg;
    GOC_DBG("on_write_dispatch: handle=%p nbufs=%u ch=%p\n", (void*)d->handle, d->nbufs, (void*)d->ch);
    /* Preflight: if the handle is NULL/closing or the loop is shutting down,
     * reject the write immediately.  A write submitted to a dying stream can
     * become the sole active libuv request on the loop, which trips libuv's
     * internal assertion uv__has_active_reqs(stream->loop) in
     * uv__write_callbacks after the req is unregistered. */
    if (!d->handle ||
        uv_is_closing((uv_handle_t*)d->handle) ||
        goc_loop_is_shutting_down()) {
        GOC_DBG("on_write_dispatch: preflight-reject handle=%p closing=%d shutdown=%d\n",
                (void*)d->handle,
                d->handle ? uv_is_closing((uv_handle_t*)d->handle) : -1,
                goc_loop_is_shutting_down());
        goc_put_cb(d->ch, SCALAR(UV_ECANCELED), goc_close_cb, d->ch);
        free(d);
        return;
    }
    goc_write_ctx_t*      ctx = (goc_write_ctx_t*)malloc(sizeof(goc_write_ctx_t));
    if (!ctx) {
        goc_put_cb(d->ch, SCALAR(UV_ENOMEM), goc_close_cb, d->ch);
        free(d);
        return;
    }
    ctx->ch = d->ch;
    ctx->nbufs = d->nbufs;
    ctx->bufs_copy = (uv_buf_t*)calloc(d->nbufs, sizeof(uv_buf_t));
    ctx->handle = d->handle;
    ctx->canceled = 0;
    ctx->next = NULL;
    if (!ctx->bufs_copy) {
        goc_put_cb(d->ch, SCALAR(UV_ENOMEM), goc_close_cb, d->ch);
        free(ctx);
        free(d);
        return;
    }

    for (unsigned i = 0; i < d->nbufs; i++) {
        size_t len = d->bufs[i].len;
        char* mem = NULL;
        if (len > 0) {
            mem = (char*)malloc(len);
            if (!mem) {
                goc_put_cb(d->ch, SCALAR(UV_ENOMEM), goc_close_cb, d->ch);
                free_owned_bufs(ctx->bufs_copy, ctx->nbufs);
                free(ctx);
                free(d);
                return;
            }
            memcpy(mem, d->bufs[i].base, len);
        }
        ctx->bufs_copy[i] = uv_buf_init(mem, (unsigned int)len);
    }

    GOC_DBG(
            "on_write_dispatch: ENTER handle=%p loop=%p bufs=%p nbufs=%u ch=%p active=%d closing=%d shutdown=%d\n",
            (void*)d->handle,
            (void*)d->handle->loop,
            (void*)ctx->bufs_copy,
            d->nbufs,
            (void*)d->ch,
            uv_is_active((uv_handle_t*)d->handle),
            uv_is_closing((uv_handle_t*)d->handle),
            goc_loop_is_shutting_down());
    int rc = uv_write(&ctx->req, d->handle, ctx->bufs_copy, d->nbufs, on_write_cb);
    if (rc == 0) {
        atomic_fetch_add_explicit(&g_uv_active_reqs, 1, memory_order_acq_rel);
    }
    GOC_DBG(
            "on_write_dispatch: uv_write rc=%d req=%p handle=%p loop=%p ch=%p active_reqs=%ld\n",
            rc, (void*)&ctx->req, (void*)d->handle, (void*)d->handle->loop, (void*)d->ch,
            atomic_load_explicit(&g_uv_active_reqs, memory_order_relaxed));
    if (rc < 0) {
        goc_put_cb(ctx->ch, SCALAR(rc), goc_close_cb, ctx->ch);
        free_owned_bufs(ctx->bufs_copy, ctx->nbufs);
        free(ctx);
    } else {
        pending_write_add(ctx);
    }
    free(d);
}

goc_chan* goc_io_write(uv_stream_t* handle,
                          const uv_buf_t bufs[], unsigned int nbufs)
{
    int worker_id = goc_current_worker_id();
    GOC_DBG(
            "goc_io_write: current_worker=%d handle=%p loop=%p nbufs=%u\n",
            worker_id, (void*)handle, (void*)handle->loop, nbufs);
    goc_chan*             ch = goc_chan_make(1);
    /* Allocate the dispatch struct with a trailing copy of the bufs array so
     * that on_write_dispatch does not dereference a pointer that may have
     * come from the caller's stack frame (which could be gone by the time the
     * async fires). */
    goc_write_dispatch_t* d  = (goc_write_dispatch_t*)malloc(
                                    sizeof(goc_write_dispatch_t) +
                                    nbufs * sizeof(uv_buf_t));
    uv_buf_t* bufs_copy = (uv_buf_t*)((char*)d + sizeof(goc_write_dispatch_t));
    memcpy(bufs_copy, bufs, nbufs * sizeof(uv_buf_t));
    d->handle = handle;
    d->bufs   = bufs_copy;
    d->nbufs  = nbufs;
    d->ch     = ch;
    dispatch_on_handle_loop(handle->loop, on_write_dispatch, d);
    GOC_DBG("goc_io_write: dispatched write handle=%p loop=%p nbufs=%u ch=%p\n",
            (void*)handle, (void*)handle->loop, nbufs, (void*)ch);
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_write2  (IPC streams)
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_write_t   req;         /* MUST be first member */
    goc_chan*    ch;
    uv_buf_t*    bufs_copy;
    unsigned     nbufs;
} goc_write2_ctx_t;

/* Reuse on_write_cb for write2 (same signature, same semantics). */
static void on_write2_cb(uv_write_t* req, int status)
{
    goc_write2_ctx_t* ctx = (goc_write2_ctx_t*)req;
    goc_put_cb(ctx->ch, SCALAR(status), goc_close_cb, ctx->ch);
    free_owned_bufs(ctx->bufs_copy, ctx->nbufs);
    free(ctx);
}

typedef struct {
    uv_stream_t*    handle;
    const uv_buf_t* bufs;
    unsigned int    nbufs;
    uv_stream_t*    send_handle;
    goc_chan*       ch;
} goc_write2_dispatch_t;

static void on_write2_dispatch(void* arg)
{
    goc_write2_dispatch_t* d   = (goc_write2_dispatch_t*)arg;
    if (!d->handle ||
        uv_is_closing((uv_handle_t*)d->handle) ||
        goc_loop_is_shutting_down()) {
        goc_put_cb(d->ch, SCALAR(UV_ECANCELED), goc_close_cb, d->ch);
        return;
    }
    goc_write2_ctx_t*      ctx = (goc_write2_ctx_t*)malloc(
                                     sizeof(goc_write2_ctx_t));
    if (!ctx) {
        goc_put_cb(d->ch, SCALAR(UV_ENOMEM), goc_close_cb, d->ch);
        return;
    }
    ctx->ch = d->ch;
    ctx->nbufs = d->nbufs;
    ctx->bufs_copy = (uv_buf_t*)calloc(d->nbufs, sizeof(uv_buf_t));
    if (!ctx->bufs_copy) {
        goc_put_cb(d->ch, SCALAR(UV_ENOMEM), goc_close_cb, d->ch);
        free(ctx);
        return;
    }

    for (unsigned i = 0; i < d->nbufs; i++) {
        size_t len = d->bufs[i].len;
        char* mem = NULL;
        if (len > 0) {
            mem = (char*)malloc(len);
            if (!mem) {
                goc_put_cb(d->ch, SCALAR(UV_ENOMEM), goc_close_cb, d->ch);
                free_owned_bufs(ctx->bufs_copy, ctx->nbufs);
                free(ctx);
                return;
            }
            memcpy(mem, d->bufs[i].base, len);
        }
        ctx->bufs_copy[i] = uv_buf_init(mem, (unsigned int)len);
    }

    int rc = uv_write2(&ctx->req, d->handle, ctx->bufs_copy, d->nbufs,
                       d->send_handle, on_write2_cb);
    if (rc < 0) {
        goc_put_cb(ctx->ch, SCALAR(rc), goc_close_cb, ctx->ch);
        free_owned_bufs(ctx->bufs_copy, ctx->nbufs);
        free(ctx);
    }
}

goc_chan* goc_io_write2(uv_stream_t* handle,
                           const uv_buf_t bufs[], unsigned int nbufs,
                           uv_stream_t* send_handle)
{
    goc_chan*              ch = goc_chan_make(1);
    goc_write2_dispatch_t* d  = (goc_write2_dispatch_t*)goc_malloc(
                                     sizeof(goc_write2_dispatch_t) +
                                     nbufs * sizeof(uv_buf_t));
    uv_buf_t* bufs_copy = (uv_buf_t*)((char*)d + sizeof(goc_write2_dispatch_t));
    memcpy(bufs_copy, bufs, nbufs * sizeof(uv_buf_t));
    d->handle      = handle;
    d->bufs        = bufs_copy;
    d->nbufs       = nbufs;
    d->send_handle = send_handle;
    d->ch          = ch;
    dispatch_on_handle_loop(handle->loop, on_write2_dispatch, d);
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
    atomic_fetch_sub_explicit(&g_uv_active_reqs, 1, memory_order_acq_rel);
    GOC_DBG("on_shutdown_cb: req=%p status=%d ch=%p handle=%p active=%d closing=%d active_reqs=%ld\n",
            (void*)req,
            status,
            (void*)ctx->ch,
            (void*)ctx->req.handle,
            ctx->req.handle ? uv_is_active((uv_handle_t*)ctx->req.handle) : -1,
            ctx->req.handle ? uv_is_closing((uv_handle_t*)ctx->req.handle) : -1,
            atomic_load_explicit(&g_uv_active_reqs, memory_order_relaxed));
    goc_put_cb(ctx->ch, SCALAR(status), goc_close_cb, ctx->ch);
    free(ctx);
}

typedef struct {
    uv_stream_t* handle;
    goc_chan*    ch;
} goc_shutdown_dispatch_t;

static void on_shutdown_dispatch(void* arg)
{
    goc_shutdown_dispatch_t* d   = (goc_shutdown_dispatch_t*)arg;
    goc_shutdown_ctx_t*      ctx = (goc_shutdown_ctx_t*)malloc(
                                       sizeof(goc_shutdown_ctx_t));
    if (!ctx) {
        goc_put_cb(d->ch, SCALAR(UV_ENOMEM), goc_close_cb, d->ch);
        free(d);
        return;
    }
    ctx->ch = d->ch;
    GOC_DBG("on_shutdown_dispatch: handle=%p loop=%p active=%d closing=%d shutdown=%d\n",
            (void*)d->handle,
            (void*)d->handle->loop,
            d->handle ? uv_is_active((uv_handle_t*)d->handle) : -1,
            d->handle ? uv_is_closing((uv_handle_t*)d->handle) : -1,
            goc_loop_is_shutting_down());
    int rc = uv_shutdown(&ctx->req, d->handle, on_shutdown_cb);
    if (rc == 0) {
        atomic_fetch_add_explicit(&g_uv_active_reqs, 1, memory_order_acq_rel);
    }
    GOC_DBG("on_shutdown_dispatch: uv_shutdown rc=%d req=%p active_reqs=%ld\n",
            rc, (void*)&ctx->req,
            atomic_load_explicit(&g_uv_active_reqs, memory_order_relaxed));
    if (rc < 0) {
        goc_put_cb(ctx->ch, SCALAR(rc), goc_close_cb, ctx->ch);
        free(ctx);
    }
    free(d);
}

goc_chan* goc_io_shutdown_stream(uv_stream_t* handle)
{
    goc_chan*                ch = goc_chan_make(1);
    goc_shutdown_dispatch_t* d  = (goc_shutdown_dispatch_t*)malloc(
                                      sizeof(goc_shutdown_dispatch_t));
    d->handle = handle;
    d->ch     = ch;
    dispatch_on_handle_loop(handle->loop, on_shutdown_dispatch, d);
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_tcp_connect
 * ---------------------------------------------------------------------- */

typedef struct {
    uv_connect_t req;   /* MUST be first member */
    goc_chan*    ch;
    int          inflight_tracked;
} goc_connect_ctx_t;

/* Soft guardrail: avoid feeding unbounded connect bursts into libuv under
 * teardown/churn stress. Above this threshold, new connect submits fail-fast
 * with UV_EBUSY so callers receive a normal error instead of destabilizing
 * the loop's internal request accounting. */
#define GOC_CONN_INFLIGHT_SOFT_MAX 128L

static _Atomic long g_tcp_connect_inflight = 0;

static void on_connect_cb(uv_connect_t* req, int status)
{
    goc_connect_ctx_t* ctx = (goc_connect_ctx_t*)req;
    if (ctx->inflight_tracked) {
        long prev = atomic_fetch_sub_explicit(&g_tcp_connect_inflight,
                                              1,
                                              memory_order_acq_rel);
        if (prev <= 0)
            atomic_store_explicit(&g_tcp_connect_inflight,
                                  0,
                                  memory_order_release);
    }
    atomic_fetch_sub_explicit(&g_uv_active_reqs, 1, memory_order_acq_rel);
    GOC_DBG("on_connect_cb: req=%p status=%d ch=%p inflight_tracked=%d inflight=%ld active_reqs=%ld\n",
            (void*)req,
            status,
            (void*)ctx->ch,
            ctx->inflight_tracked,
            atomic_load_explicit(&g_tcp_connect_inflight, memory_order_relaxed),
            atomic_load_explicit(&g_uv_active_reqs, memory_order_relaxed));
    goc_put_cb(ctx->ch, SCALAR(status), goc_close_cb, ctx->ch);
    free(ctx);
}

typedef struct {
    uv_tcp_t*           handle;
    struct sockaddr_storage addr; /* copy of the target address */
    goc_chan*           ch;
} goc_tcp_connect_dispatch_t;

static int connect_dispatch_preflight(uv_handle_t* handle)
{
    if (!handle)
        return UV_EINVAL;
    if (handle->type != UV_TCP)
        return UV_EINVAL;
    if (handle->loop == NULL)
        return UV_EINVAL;
    /* Note: handle may be on a worker loop — do not reject non-g_loop handles. */
    if (goc_loop_is_shutting_down())
        return UV_ECANCELED;
    if (uv_is_closing(handle))
        return UV_ECANCELED;
    return 0;
}

static int connect_dispatch_overloaded(void)
{
    long in = atomic_load_explicit(&g_tcp_connect_inflight, memory_order_relaxed);
    return (in >= GOC_CONN_INFLIGHT_SOFT_MAX);
}

static void on_tcp_connect_dispatch(void* arg)
{
    goc_tcp_connect_dispatch_t* d   = (goc_tcp_connect_dispatch_t*)arg;
    GOC_DBG("on_tcp_connect_dispatch: handle=%p ch=%p loop=%p closing=%d active=%d\n",
            (void*)d->handle, (void*)d->ch,
            d->handle ? (void*)d->handle->loop : NULL,
            d->handle ? uv_is_closing((uv_handle_t*)d->handle) : -1,
            d->handle ? uv_is_active((uv_handle_t*)d->handle) : -1);
    int preflight_rc = connect_dispatch_preflight((uv_handle_t*)d->handle);
    if (preflight_rc < 0) {
        GOC_DBG("on_tcp_connect_dispatch: rejecting connect handle=%p rc=%d shutdown=%d\n",
                (void*)d->handle, preflight_rc, goc_loop_is_shutting_down());
        goc_put_cb(d->ch, SCALAR(preflight_rc), goc_close_cb, d->ch);
        free(d);
        return;
    }
    if (connect_dispatch_overloaded()) {
        GOC_DBG("on_tcp_connect_dispatch: rejecting connect handle=%p rc=%d inflight=%ld max=%ld\n",
                (void*)d->handle,
                UV_EBUSY,
                atomic_load_explicit(&g_tcp_connect_inflight, memory_order_relaxed),
                GOC_CONN_INFLIGHT_SOFT_MAX);
        goc_put_cb(d->ch, SCALAR(UV_EBUSY), goc_close_cb, d->ch);
        free(d);
        return;
    }
    goc_connect_ctx_t*          ctx = (goc_connect_ctx_t*)malloc(
                                          sizeof(goc_connect_ctx_t));
    if (!ctx) {
        goc_put_cb(d->ch, SCALAR(UV_ENOMEM), goc_close_cb, d->ch);
        free(d);
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->ch = d->ch;
    ctx->inflight_tracked = 0;

    char addrbuf[INET6_ADDRSTRLEN] = {0};
    int port = 0;
    if (d->addr.ss_family == AF_INET) {
        struct sockaddr_in* sa = (struct sockaddr_in*)&d->addr;
        uv_ip4_name(sa, addrbuf, sizeof(addrbuf));
        port = ntohs(sa->sin_port);
    } else if (d->addr.ss_family == AF_INET6) {
        struct sockaddr_in6* sa6 = (struct sockaddr_in6*)&d->addr;
        uv_ip6_name(sa6, addrbuf, sizeof(addrbuf));
        port = ntohs(sa6->sin6_port);
    } else {
        strncpy(addrbuf, "<unknown>", sizeof(addrbuf) - 1);
        addrbuf[sizeof(addrbuf) - 1] = '\0';
    }
    GOC_DBG("on_tcp_connect_dispatch: pre-connect handle=%p loop=%p closing=%d active=%d dst=%s port=%d family=%d\n",
            (void*)d->handle,
            d->handle ? (void*)d->handle->loop : NULL,
            d->handle ? uv_is_closing((uv_handle_t*)d->handle) : -1,
            d->handle ? uv_is_active((uv_handle_t*)d->handle) : -1,
            addrbuf,
            port,
            d->addr.ss_family);
    int rc = uv_tcp_connect(&ctx->req, d->handle,
                            (const struct sockaddr*)&d->addr,
                            on_connect_cb);
    if (rc < 0) {
        GOC_DBG("on_tcp_connect_dispatch: uv_tcp_connect failed handle=%p dst=%s port=%d family=%d rc=%d err=%s\n",
                (void*)d->handle,
                addrbuf,
                port,
                d->addr.ss_family,
                rc,
                uv_strerror(rc));
    }
    if (rc == 0) {
        ctx->inflight_tracked = 1;
        atomic_fetch_add_explicit(&g_tcp_connect_inflight,
                                  1,
                                  memory_order_acq_rel);
        atomic_fetch_add_explicit(&g_uv_active_reqs, 1, memory_order_acq_rel);
    }
    GOC_DBG("on_tcp_connect_dispatch: uv_tcp_connect rc=%d req=%p handle=%p inflight=%ld active_reqs=%ld\n",
            rc, (void*)&ctx->req, (void*)d->handle,
            atomic_load_explicit(&g_tcp_connect_inflight, memory_order_relaxed),
            atomic_load_explicit(&g_uv_active_reqs, memory_order_relaxed));
    if (rc < 0) {
        ctx->inflight_tracked = 0;
        goc_put_cb(ctx->ch, SCALAR(rc), goc_close_cb, ctx->ch);
        free(ctx);
    }
    free(d);
}

goc_chan* goc_io_tcp_connect(uv_tcp_t* handle, const struct sockaddr* addr)
{
    goc_chan*                   ch = goc_chan_make(1);
    goc_chan_set_debug_tag(ch, "goc_tcp_connect_ch");
    GOC_DBG("goc_io_tcp_connect: dispatch handle=%p current_loop=%p target_loop=%p\n",
            (void*)handle,
            (void*)goc_current_worker_loop(),
            (void*)((uv_handle_t*)handle)->loop);
    goc_tcp_connect_dispatch_t* d  = (goc_tcp_connect_dispatch_t*)malloc(
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
    if (goc_loop_is_shutting_down()) {
        goc_put_cb(ch, SCALAR(UV_ECANCELED), goc_close_cb, ch);
        free(d);
    } else {
        dispatch_on_handle_loop(((uv_handle_t*)handle)->loop,
                                on_tcp_connect_dispatch, d);
    }
    return ch;
}


/* -------------------------------------------------------------------------
 * goc_io_pipe_connect
 * ---------------------------------------------------------------------- */

/* uv_pipe_connect has no return code; the callback always fires. */

typedef struct {
    uv_pipe_t*   handle;
    char*        name;     /* malloc-copied pipe name */
    goc_chan*    ch;
} goc_pipe_connect_dispatch_t;

static void on_pipe_connect_dispatch(void* arg)
{
    goc_pipe_connect_dispatch_t* d   = (goc_pipe_connect_dispatch_t*)arg;
    int preflight_rc = connect_dispatch_preflight((uv_handle_t*)d->handle);
    if (preflight_rc < 0) {
        GOC_DBG("on_pipe_connect_dispatch: rejecting connect handle=%p rc=%d shutdown=%d\n",
                (void*)d->handle, preflight_rc, goc_loop_is_shutting_down());
        goc_put_cb(d->ch, SCALAR(preflight_rc), goc_close_cb, d->ch);
        return;
    }
    goc_connect_ctx_t*           ctx = (goc_connect_ctx_t*)malloc(
                                           sizeof(goc_connect_ctx_t));
    if (!ctx) {
        goc_put_cb(d->ch, SCALAR(UV_ENOMEM), goc_close_cb, d->ch);
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->ch = d->ch;
    uv_pipe_connect(&ctx->req, d->handle, d->name, on_connect_cb);
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
    if (goc_loop_is_shutting_down()) {
        goc_put_cb(ch, SCALAR(UV_ECANCELED), goc_close_cb, ch);
    } else {
        dispatch_on_handle_loop(((uv_handle_t*)handle)->loop,
                                on_pipe_connect_dispatch, d);
    }
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
    goc_put_cb(ctx->ch, SCALAR(status), goc_close_cb, ctx->ch);
}

typedef struct {
    uv_udp_t*               handle;
    const uv_buf_t*         bufs;
    unsigned int            nbufs;
    struct sockaddr_storage addr;     /* copy of destination address */
    goc_chan*               ch;
} goc_udp_send_dispatch_t;

static void on_udp_send_dispatch(void* arg)
{
    goc_udp_send_dispatch_t* d   = (goc_udp_send_dispatch_t*)arg;
    goc_udp_send_ctx_t*      ctx = (goc_udp_send_ctx_t*)goc_malloc(
                                       sizeof(goc_udp_send_ctx_t));
    ctx->ch = d->ch;
    int rc = uv_udp_send(&ctx->req, d->handle, d->bufs, d->nbufs,
                         (const struct sockaddr*)&d->addr, on_udp_send_cb);
    if (rc < 0) {
        goc_put_cb(ctx->ch, SCALAR(rc), goc_close_cb, ctx->ch);
    } else {
        gc_handle_register(ctx);
    }
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
    dispatch_on_handle_loop(((uv_handle_t*)d->handle)->loop, on_udp_send_dispatch, d);
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
    goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
    handle->data = NULL;
}

typedef struct {
    uv_udp_t*   handle;
    goc_chan*   ch;
} goc_udp_recv_start_dispatch_t;

static void on_udp_recv_start_dispatch(void* arg)
{
    goc_udp_recv_start_dispatch_t* d = (goc_udp_recv_start_dispatch_t*)arg;

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
        goc_put_cb(d->ch, res, goc_close_cb, d->ch);
        d->handle->data = NULL;
    }
    free(d);
}

goc_chan* goc_io_udp_recv_start(uv_udp_t* handle)
{
    goc_chan*                      ch = goc_chan_make(16);
    goc_udp_recv_start_dispatch_t* d  = (goc_udp_recv_start_dispatch_t*)malloc(
                                            sizeof(goc_udp_recv_start_dispatch_t));
    d->handle = handle;
    d->ch     = ch;
    dispatch_on_handle_loop(((uv_handle_t*)handle)->loop, on_udp_recv_start_dispatch, d);
    return ch;
}

typedef struct {
    uv_udp_t*   handle;
} goc_udp_recv_stop_dispatch_t;

static void on_udp_recv_stop_dispatch(void* arg)
{
    goc_udp_recv_stop_dispatch_t* d = (goc_udp_recv_stop_dispatch_t*)arg;

    uv_udp_recv_stop(d->handle);

    if (d->handle->data) {
        goc_udp_recv_ctx_t* ctx = (goc_udp_recv_ctx_t*)d->handle->data;
        goc_close(ctx->ch);
        d->handle->data = NULL;
    }
    free(d);
}

int goc_io_udp_recv_stop(uv_udp_t* handle)
{
    goc_udp_recv_stop_dispatch_t* d = (goc_udp_recv_stop_dispatch_t*)malloc(
                                          sizeof(goc_udp_recv_stop_dispatch_t));
    d->handle = handle;
    dispatch_on_handle_loop(((uv_handle_t*)handle)->loop, on_udp_recv_stop_dispatch, d);
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
 * When called from a pool-worker fiber, initialise the handle directly on
 * the worker's own uv_loop_t — no cross-thread dispatch needed.  The handle
 * is then owned by that worker's loop; subsequent I/O callbacks will fire on
 * the same worker thread.  The home loop can always be recovered via
 * ((uv_handle_t*)handle)->loop after a successful init.
 *
 * When called from outside a worker (main thread, g_loop callbacks), or for
 * handles that must stay on g_loop (fs_event, fs_poll), dispatch to the loop
 * thread via post_on_loop as before.
 *
 * On success the handle is registered as a GC root.
 * ---------------------------------------------------------------------- */

typedef struct {
    void*        handle;
    int          ipc;     /* pipe only */
    uv_file      fd;      /* tty only */
    goc_chan*    ch;
    int          kind;    /* 0=tcp 1=pipe 2=udp 3=tty 4=signal 5=fs_event 6=fs_poll */
    uv_loop_t*   target_loop;
} goc_io_handle_init_dispatch_t;

static void on_handle_init_dispatch(void* arg)
{
    goc_io_handle_init_dispatch_t* d = (goc_io_handle_init_dispatch_t*)arg;
    int rc;
    switch (d->kind) {
        case 0: rc = uv_tcp_init(d->target_loop,      (uv_tcp_t*)d->handle);            break;
        case 1: rc = uv_pipe_init(d->target_loop,     (uv_pipe_t*)d->handle, d->ipc);   break;
        case 2: rc = uv_udp_init(d->target_loop,      (uv_udp_t*)d->handle);            break;
        case 3: rc = uv_tty_init(d->target_loop,      (uv_tty_t*)d->handle, d->fd, 0); break;
        case 4: rc = uv_signal_init(d->target_loop,   (uv_signal_t*)d->handle);         break;
        case 5: rc = uv_fs_event_init(d->target_loop, (uv_fs_event_t*)d->handle);       break;
        case 6: rc = uv_fs_poll_init(d->target_loop,  (uv_fs_poll_t*)d->handle);        break;
        default: rc = UV_EINVAL; break;
    }
    uv_handle_t* uv_handle = (uv_handle_t*)d->handle;
    goc_uv_init_log("on_handle_init_dispatch", rc, d->target_loop, uv_handle);
    if (rc == 0) {
        gc_handle_register(uv_handle);
        goc_io_handle_t* wrapper = goc_io_handle_wrap(uv_handle, NULL);
        if (uv_handle->data != NULL && uv_handle->data != wrapper) {
            GOC_DBG("handle_init_dispatch: overwriting existing handle->data=%p on init target=%p\n",
                    uv_handle->data, (void*)uv_handle);
        }
        uv_handle->data = wrapper;
    }
    goc_put_cb(d->ch, SCALAR(rc), goc_close_cb, d->ch);
    free(d);
}

static goc_chan* handle_init_dispatch(void* handle, int kind, int ipc, uv_file fd,
                                      bool worker_affine)
{
    goc_chan* ch = goc_chan_make(1);
    switch (kind) {
        case 0: goc_chan_set_debug_tag(ch, "goc_tcp_init_ch"); break;
        case 1: goc_chan_set_debug_tag(ch, "goc_pipe_init_ch"); break;
        case 2: goc_chan_set_debug_tag(ch, "goc_udp_init_ch"); break;
        case 3: goc_chan_set_debug_tag(ch, "goc_tty_init_ch"); break;
        case 4: goc_chan_set_debug_tag(ch, "goc_signal_init_ch"); break;
        case 5: goc_chan_set_debug_tag(ch, "goc_fs_event_init_ch"); break;
        case 6: goc_chan_set_debug_tag(ch, "goc_fs_poll_init_ch"); break;
        default: break;
    }

    uv_loop_t* target_loop = worker_affine ? goc_worker_or_default_loop() : g_loop;

    if (worker_affine && target_loop != g_loop) {
        /* Fast path: called from a worker fiber — init directly on this
         * worker's loop (safe: we ARE the loop thread for our own loop). */
        int rc;
        uv_handle_t* uv_handle = (uv_handle_t*)handle;
        switch (kind) {
            case 0: rc = uv_tcp_init(target_loop,    (uv_tcp_t*)handle);           break;
            case 1: rc = uv_pipe_init(target_loop,   (uv_pipe_t*)handle, ipc);     break;
            case 2: rc = uv_udp_init(target_loop,    (uv_udp_t*)handle);           break;
            case 3: rc = uv_tty_init(target_loop,    (uv_tty_t*)handle, fd, 0);   break;
            case 4: rc = uv_signal_init(target_loop, (uv_signal_t*)handle);        break;
            default: rc = UV_EINVAL; break;
        }
        goc_uv_init_log("handle_init_dispatch fast path", rc, target_loop, uv_handle);
        if (rc == 0) {
            gc_handle_register(uv_handle);
            goc_io_handle_t* wrapper = goc_io_handle_wrap(uv_handle, NULL);
            if (uv_handle->data != NULL && uv_handle->data != wrapper) {
                GOC_DBG("handle_init_dispatch fast path: overwriting existing handle->data=%p on init target=%p\n",
                        uv_handle->data, (void*)uv_handle);
            }
            uv_handle->data = wrapper;
        }
        goc_put_cb(ch, SCALAR(rc), goc_close_cb, ch);
        return ch;
    }

    /* Slow path: dispatch to the owning loop thread. */
    goc_io_handle_init_dispatch_t* d = (goc_io_handle_init_dispatch_t*)malloc(
                                        sizeof(goc_io_handle_init_dispatch_t));
    d->handle      = handle;
    d->kind        = kind;
    d->ipc         = ipc;
    d->fd          = fd;
    d->ch          = ch;
    d->target_loop = target_loop;
    post_on_loop(on_handle_init_dispatch, d);
    return ch;
}

goc_chan* goc_io_tcp_init(uv_tcp_t* handle)
{
    return handle_init_dispatch(handle, 0, 0, 0, true);
}

goc_chan* goc_io_pipe_init(uv_pipe_t* handle, int ipc)
{
    return handle_init_dispatch(handle, 1, ipc, 0, true);
}

goc_chan* goc_io_udp_init(uv_udp_t* handle)
{
    return handle_init_dispatch(handle, 2, 0, 0, true);
}

goc_chan* goc_io_tty_init(uv_tty_t* handle, uv_file fd)
{
    return handle_init_dispatch(handle, 3, 0, fd, true);
}

goc_chan* goc_io_signal_init(uv_signal_t* handle)
{
    return handle_init_dispatch(handle, 4, 0, 0, true);
}

goc_chan* goc_io_fs_event_init(uv_fs_event_t* handle)
{
    return handle_init_dispatch(handle, 5, 0, 0, false);
}

goc_chan* goc_io_fs_poll_init(uv_fs_poll_t* handle)
{
    return handle_init_dispatch(handle, 6, 0, 0, false);
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
        goc_put_cb(ctx->exit_ch, res, goc_close_cb, ctx->exit_ch);
    }
}

typedef struct {
    uv_process_t*            handle;
    uv_process_options_t     opts_copy; /* mutable copy */
    goc_chan*                ch;
    goc_chan*                exit_ch;  /* NULL if caller passed NULL */
} goc_process_spawn_dispatch_t;

static void on_process_spawn_dispatch(void* arg)
{
    goc_process_spawn_dispatch_t* d = (goc_process_spawn_dispatch_t*)arg;

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
    goc_put_cb(d->ch, SCALAR(rc), goc_close_cb, d->ch);
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
    post_on_loop(on_process_spawn_dispatch, d);
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

typedef struct goc_io_handle_close_reg {
    uv_handle_t* handle;
    uv_close_cb  user_cb;
    goc_chan*    ch;
    struct goc_io_handle_close_reg* next;
} goc_io_handle_close_reg_t;

typedef struct {
    uv_handle_t* handle;
    uv_close_cb  user_cb;
    goc_chan*    ch;
} goc_io_handle_close_reg_arg_t;

/* Accessed only on the owning libuv loop thread via register_handle_close_cb
 * and on_goc_io_handle_close, so no extra synchronization is needed. Each
 * loop thread keeps its own registry to avoid cross-thread corruption. */
static _Thread_local goc_io_handle_close_reg_t* g_handle_close_regs = NULL;

static void register_handle_close_cb_dispatch(void* arg)
{
    goc_io_handle_close_reg_arg_t* d = (goc_io_handle_close_reg_arg_t*)arg;
    if (!d) {
        return;
    }
    register_handle_close_cb(d->handle, d->user_cb, d->ch);
    free(d);
}

static void register_handle_close_cb_safe(uv_handle_t* handle,
                                          uv_close_cb user_cb,
                                          goc_chan* ch)
{
    if (!handle) {
        if (user_cb) {
            user_cb(handle);
        }
        if (ch) {
            goc_close(ch);
        }
        return;
    }

    if (goc_on_loop_thread() && handle->loop == goc_current_worker_loop()) {
        register_handle_close_cb(handle, user_cb, ch);
        return;
    }

    goc_io_handle_close_reg_arg_t* d =
        (goc_io_handle_close_reg_arg_t*)malloc(
            sizeof(goc_io_handle_close_reg_arg_t));
    if (!d) {
        GOC_DBG("register_handle_close_cb_safe: malloc failed handle=%p user_cb=%p ch=%p\n",
                (void*)handle, (void*)user_cb, (void*)ch);
        if (user_cb) {
            user_cb(handle);
        }
        if (ch) {
            goc_close(ch);
        }
        return;
    }

    d->handle = handle;
    d->user_cb = user_cb;
    d->ch = ch;
    dispatch_on_handle_loop(handle->loop, register_handle_close_cb_dispatch, d);
}

typedef struct {
    goc_server_ctx_t* ctx;
} goc_server_stream_close_cleanup_t;

static void on_server_stream_close_cleanup(void* arg)
{
    goc_server_stream_close_cleanup_t* d = (goc_server_stream_close_cleanup_t*)arg;
    if (!d) {
        return;
    }
    goc_server_ctx_t* ctx = d->ctx;
    if (!ctx) {
        free(d);
        return;
    }
    GOC_DBG("on_server_stream_close_cleanup: ctx=%p ch=%p\n",
            (void*)ctx, (void*)ctx->ch);
    if (goc_chan_is_open(ctx->ch)) {
        drop_buffered_accept_connections(ctx->ch);
        GOC_DBG("on_server_stream_close_cleanup: closing accept channel ch=%p ctx=%p\n",
                (void*)ctx->ch, (void*)ctx);
        goc_close(ctx->ch);
    } else {
        GOC_DBG("on_server_stream_close_cleanup: accept channel already closing ch=%p ctx=%p\n",
                (void*)ctx->ch, (void*)ctx);
    }
    free(d);
}

static void register_handle_close_cb(uv_handle_t* handle,
                                      uv_close_cb user_cb,
                                      goc_chan* ch)
{
    if (handle && uv_is_closing(handle)) {
        goc_io_handle_t* h = goc_io_handle_from_uv(handle);
        int state = h ? (int)atomic_load(&h->state) : GOC_H_CLOSED;
        if (!h || state == GOC_H_CLOSED) {
            GOC_DBG("register_handle_close_cb: handle already closed, completing immediately handle=%p user_cb=%p ch=%p state=%d\n",
                    (void*)handle, (void*)user_cb, (void*)ch, state);
            if (user_cb) {
                user_cb(handle);
            }
            if (ch) {
                goc_close(ch);
            }
            return;
        }
    }

    goc_io_handle_close_reg_t* n =
        (goc_io_handle_close_reg_t*)malloc(sizeof(goc_io_handle_close_reg_t));
    n->handle  = handle;
    n->user_cb = user_cb;
    n->ch      = ch;
    n->next    = g_handle_close_regs;
    g_handle_close_regs = n;

    size_t reg_count = 0;
    for (goc_io_handle_close_reg_t* cur = g_handle_close_regs; cur; cur = cur->next)
        reg_count++;
    GOC_DBG("register_handle_close_cb: handle=%p user_cb=%p ch=%p thread=%llu regs=%zu\n",
            (void*)handle, (void*)user_cb, (void*)ch, goc_uv_thread_id(), reg_count);
}

static goc_io_handle_close_reg_t* take_handle_close_reg(uv_handle_t* handle)
{
    goc_io_handle_close_reg_t* prev = NULL;
    goc_io_handle_close_reg_t* cur  = g_handle_close_regs;
    while (cur) {
        if (cur->handle == handle) {
            if (prev)
                prev->next = cur->next;
            else
                g_handle_close_regs = cur->next;

            size_t reg_count = 0;
            for (goc_io_handle_close_reg_t* scan = g_handle_close_regs; scan; scan = scan->next)
                reg_count++;
            GOC_DBG("take_handle_close_reg: handle=%p found user_cb=%p ch=%p remaining_regs=%zu thread=%llu\n",
                    (void*)handle, (void*)cur->user_cb, (void*)cur->ch, reg_count, goc_uv_thread_id());
            return cur;
        }
        prev = cur;
        cur  = cur->next;
    }
    GOC_DBG("take_handle_close_reg: handle=%p not found regs=%p thread=%llu\n",
            (void*)handle, (void*)g_handle_close_regs, goc_uv_thread_id());
    return NULL;
}

static void on_goc_io_handle_close(uv_handle_t* handle)
{
    goc_uv_callback_enter("on_goc_io_handle_close");
    goc_uv_close_log("on_goc_io_handle_close", handle);
    goc_uv_handle_log("on_goc_io_handle_close", handle);
    GOC_DBG("on_goc_io_handle_close: handle=%p data=%p type=%s active=%d has_ref=%d closing=%d loop=%p loop_alive=%d thread=%llu\n",
            (void*)handle,
            handle ? handle->data : NULL,
            handle ? uv_handle_type_name(uv_handle_get_type(handle)) : "<null>",
            handle ? uv_is_active(handle) : -1,
            handle ? uv_has_ref(handle) : -1,
            handle ? uv_is_closing(handle) : -1,
            handle ? (void*)handle->loop : NULL,
            handle && handle->loop ? uv_loop_alive(handle->loop) : -1,
            goc_uv_thread_id());
    void* close_data = NULL;
    if (handle)
        close_data = handle->data;
    GOC_DBG("on_goc_io_handle_close: initial close_data=%p wrapper=%d handle_loop=%p\n",
            close_data,
            close_data ? is_goc_io_handle_wrapper(close_data) : 0,
            handle ? (void*)handle->loop : NULL);

    if (handle && close_data && is_goc_io_handle_wrapper(close_data)) {
        GOC_DBG("on_goc_io_handle_close: preserving wrapper data until cleanup handle=%p data=%p type=%s\n",
                (void*)handle,
                close_data,
                uv_handle_type_name(uv_handle_get_type(handle)));
    } else if (handle) {
        GOC_DBG("on_goc_io_handle_close: clearing stale handle->data handle=%p data=%p type=%s\n",
                (void*)handle,
                handle->data,
                uv_handle_type_name(uv_handle_get_type(handle)));
        handle->data = NULL;

        goc_io_handle_t* recovered = goc_io_handle_registry_find(handle);
        if (recovered) {
            GOC_DBG("on_goc_io_handle_close: recovered wrapper from registry for handle=%p wrapper=%p\n",
                    (void*)handle, (void*)recovered);
            close_data = recovered;
            handle->data = recovered;
        }

        GOC_DBG("on_goc_io_handle_close: handle->data cleared handle=%p type=%s recovered=%p\n",
                (void*)handle,
                uv_handle_type_name(uv_handle_get_type(handle)),
                (void*)handle->data);
    }

    if (!close_data && handle) {
        goc_io_handle_t* recovered = goc_io_handle_registry_find(handle);
        if (recovered) {
            GOC_DBG("on_goc_io_handle_close: recovered wrapper from registry for handle=%p wrapper=%p\n",
                    (void*)handle, (void*)recovered);
            close_data = recovered;
            handle->data = recovered;
        } else {
            GOC_DBG("on_goc_io_handle_close: no wrapper found in registry for handle=%p; handle->data remains NULL\n",
                    (void*)handle);
        }
    }
    if (handle && close_data) {
        if (is_server_ctx(close_data)) {
            goc_server_ctx_t* ctx = (goc_server_ctx_t*)close_data;
            GOC_DBG("on_goc_io_handle_close: scheduling deferred server stream cleanup ch=%p handle=%p ctx=%p closing=%d\n",
                    (void*)ctx->ch, (void*)handle, (void*)ctx,
                    (int)goc_chan_close_state(ctx->ch));
            goc_server_stream_close_cleanup_t* d =
                (goc_server_stream_close_cleanup_t*)malloc(
                    sizeof(goc_server_stream_close_cleanup_t));
            d->ctx = ctx;
            if (handle->loop == g_loop) {
                post_on_loop(on_server_stream_close_cleanup, d);
                GOC_DBG("on_goc_io_handle_close: deferred server cleanup queued on g_loop ch=%p handle=%p\n",
                        (void*)ctx->ch, (void*)handle);
            } else {
                int rc = post_on_handle_loop(handle->loop,
                                             on_server_stream_close_cleanup,
                                             d);
                if (rc < 0) {
                    GOC_DBG("on_goc_io_handle_close: deferred cleanup post failed rc=%d, scheduling close ch=%p handle=%p\n",
                            rc, (void*)ctx->ch, (void*)handle);
                    if (goc_chan_is_open(ctx->ch)) {
                        drop_buffered_accept_connections(ctx->ch);
                        GOC_DBG("on_goc_io_handle_close: fallback close_chan_if_not_closing for ctx->ch=%p handle=%p\n",
                                (void*)ctx->ch, (void*)handle);
                        goc_close(ctx->ch);
                    }
                    free(d);
                } else {
                    GOC_DBG("on_goc_io_handle_close: deferred server cleanup queued ch=%p handle=%p\n",
                            (void*)ctx->ch, (void*)handle);
                }
            }
        } else if (is_goc_io_handle_wrapper(close_data)) {
            goc_io_handle_t* h = (goc_io_handle_t*)close_data;
            GOC_DBG("on_goc_io_handle_close: deferring wrapper cleanup for handle=%p wrapper=%p state=%d\n",
                    (void*)handle, (void*)h, (int)h->state);
            h->state = GOC_H_CLOSED;
            goc_worker_io_handle_closed();
            goc_io_handle_t* removed = goc_io_handle_registry_remove(handle);
            GOC_DBG("on_goc_io_handle_close: registry_remove returned=%p for handle=%p wrapper=%p\n",
                    (void*)removed, (void*)handle, (void*)h);
            handle->data = NULL;
            goc_deferred_wrapper_free_add(h);
            GOC_DBG("on_goc_io_handle_close: wrapper cleanup deferred for handle=%p wrapper=%p\n",
                    (void*)handle, (void*)h);
        } else {
            GOC_DBG("on_goc_io_handle_close: handle->data still present %p on close callback for handle=%p\n",
                    close_data, (void*)handle);
        }
    }

    goc_io_handle_close_reg_t* reg;
    bool saw_any_cb = false;
    while ((reg = take_handle_close_reg(handle)) != NULL) {
        if (reg->user_cb) {
            GOC_DBG("on_goc_io_handle_close: user_cb=%p handle=%p\n",
                    (void*)reg->user_cb, (void*)handle);
            saw_any_cb = true;
            reg->user_cb(handle);
        }
        if (reg->ch) {
            saw_any_cb = true;
            GOC_DBG("on_goc_io_handle_close: join channel close for handle=%p ch=%p\n",
                    (void*)handle, (void*)reg->ch);
            goc_close(reg->ch);
        }
        free(reg);
    }
    if (!saw_any_cb) {
        GOC_DBG("on_goc_io_handle_close: no user close callback registered for handle=%p\n",
                (void*)handle);
    }
    GOC_DBG("on_goc_io_handle_close: after unregister handle=%p loop_alive=%d\n",
            (void*)handle,
            handle && handle->loop ? uv_loop_alive(handle->loop) : -1);
    if (handle && handle->loop) {
        goc_uv_walk_log(handle->loop, "on_goc_io_handle_close after unregister");
    }

    GOC_DBG("on_goc_io_handle_close: queue handle unregister handle=%p close_data=%p loop=%p\n",
            (void*)handle, close_data, handle ? (void*)handle->loop : NULL);
    goc_deferred_handle_unreg_add(handle);
    if (!goc_on_loop_thread()) {
        GOC_DBG("on_goc_io_handle_close: waking loop from non-loop thread for handle=%p\n",
                (void*)handle);
        goc_loop_wakeup();
    }
    GOC_DBG("on_goc_io_handle_close: exit handle=%p\n", (void*)handle);
    goc_uv_callback_exit("on_goc_io_handle_close");
}

#define SAFE_UV_CLOSE(handle, cb)                                 \
    do {                                                          \
        if (goc_uv_callback_depth > 0) {                           \
            ABORT("SAFE_UV_CLOSE called during callback depth=%d handle=%p\n", \
                  goc_uv_callback_depth, (void*)(handle));         \
        }                                                         \
        uv_close((handle), (cb));                                  \
    } while (0)

typedef struct {
    uv_handle_t* target;
    uv_close_cb  user_cb;
    goc_chan*    ch;
} goc_io_handle_close_dispatch_t;

static void on_handle_close_dispatch(void* arg)
{
    goc_uv_callback_enter("on_handle_close_dispatch");
    goc_io_handle_close_dispatch_t* d = (goc_io_handle_close_dispatch_t*)arg;
    goc_uv_close_log("on_handle_close_dispatch entry", d->target);
    GOC_DBG("on_handle_close_dispatch: entry arg=%p target=%p\n",
            arg,
            (void*)d->target);
    GOC_DBG(
            "on_handle_close_dispatch: target=%p loop=%p type=%s active=%d has_ref=%d closing=%d data=%p\n",
            (void*)d->target,
            d->target ? (void*)d->target->loop : NULL,
            d->target ? uv_handle_type_name(uv_handle_get_type(d->target)) : "<null>",
            d->target ? uv_is_active(d->target) : -1,
            d->target ? uv_has_ref(d->target) : -1,
            (!d->target) ? -1 : uv_is_closing(d->target),
            d->target ? d->target->data : NULL);
    if (d->target) {
        GOC_DBG(
                "on_handle_close_dispatch: target=%p data=%p loop=%p type=%s\n",
                (void*)d->target,
                d->target->data,
                (void*)d->target->loop,
                uv_handle_type_name(uv_handle_get_type(d->target)));
    }
    if (!d->target) {
        GOC_DBG("on_handle_close_dispatch: NULL target, skipping uv_close\n");
        GOC_DBG("on_handle_close_dispatch: exit target=NULL\n");
        free(d);
        goc_uv_callback_exit("on_handle_close_dispatch");
        return;
    }
    if (uv_is_closing(d->target)) {
        GOC_DBG(
                "on_handle_close_dispatch: target already closing, registering callback only target=%p data=%p loop=%p loop_alive=%d type=%s active=%d has_ref=%d\n",
                (void*)d->target,
                d->target->data,
                (void*)d->target->loop,
                d->target->loop ? uv_loop_alive(d->target->loop) : -1,
                uv_handle_type_name(d->target->type),
                uv_is_active(d->target),
                uv_has_ref(d->target));
        if (d->user_cb || d->ch)
            register_handle_close_cb_safe(d->target, d->user_cb, d->ch);
        GOC_DBG("on_handle_close_dispatch: exit target=%p early-closing\n", (void*)d->target);
        free(d);
        goc_uv_callback_exit("on_handle_close_dispatch");
        return;
    }
    /* Extra hardening: reject handles that were never properly initialized.
     * type==UV_UNKNOWN_HANDLE or loop==NULL means uv_tcp_init was never called
     * (or the handle was zeroed/freed), and passing such a handle to uv_close
     * would violate libuv's uv__has_active_reqs invariant. */
    if (d->target->type == UV_UNKNOWN_HANDLE || d->target->loop == NULL) {
        ABORT("on_handle_close_dispatch: invalid/uninitialized target=%p type=%s(%d) loop=%p closing=%d data=%p\n",
              (void*)d->target,
              d->target ? uv_handle_type_name(d->target->type) : "<null>",
              (int)d->target->type,
              (void*)d->target->loop,
              d->target ? uv_is_closing(d->target) : -1,
              d->target ? d->target->data : NULL);
    }

    if (!uv_loop_alive(d->target->loop)) {
        GOC_DBG(
                "on_handle_close_dispatch: target loop dead, skipping uv_close target=%p data=%p loop=%p loop_alive=%d type=%s active=%d has_ref=%d\n",
                (void*)d->target,
                d->target->data,
                (void*)d->target->loop,
                uv_loop_alive(d->target->loop),
                uv_handle_type_name(d->target->type),
                uv_is_active(d->target),
                uv_has_ref(d->target));
        GOC_DBG("on_handle_close_dispatch: exit target=%p early-loop-dead\n", (void*)d->target);
        free(d);
        goc_uv_callback_exit("on_handle_close_dispatch");
        return;
    }

    uv_handle_type type = d->target->type;
    if (type == UV_TCP || type == UV_NAMED_PIPE || type == UV_TTY) {
        uv_stream_t* stream = (uv_stream_t*)d->target;
        GOC_DBG("on_handle_close_dispatch: stream cleanup start stream=%p data=%p\n",
                (void*)stream, stream->data);
        goc_read_start_pending_t* pending =
            pending_read_start_take_for_stream(stream);
        GOC_DBG("on_handle_close_dispatch: pending read-start head=%p stream=%p\n",
                (void*)pending, (void*)stream);
        if (!pending) {
            GOC_DBG("on_handle_close_dispatch: no pending read-starts for stream=%p\n",
                    (void*)stream);
        }
        size_t canceled_pending = 0;
        while (pending) {
            goc_read_start_pending_t* next = pending->next;
            GOC_DBG("on_handle_close_dispatch: canceling pending read-start stream=%p ch=%p pending=%p stream->data=%p\n",
                    (void*)stream, (void*)pending->ch, (void*)pending, stream->data);
            pending->canceled = 1;
            goc_io_read_t* res = (goc_io_read_t*)goc_malloc(sizeof(goc_io_read_t));
            res->nread = UV_ECANCELED;
            res->buf   = NULL;
            goc_put_cb(pending->ch, res, goc_close_cb, pending->ch);
            GOC_DBG("on_handle_close_dispatch: canceled pending read-start stream=%p pending=%p ch=%p\n",
                    (void*)stream, (void*)pending, (void*)pending->ch);
            pending = next;
            canceled_pending++;
        }
        if (canceled_pending > 0) {
            GOC_DBG("on_handle_close_dispatch: canceled %zu pending read-start(s) for stream=%p\n",
                    canceled_pending, (void*)stream);
        }

        GOC_DBG("on_handle_close_dispatch: canceling pending writes for stream=%p data=%p\n",
                (void*)stream, stream->data);
        pending_write_cancel_for_handle(stream);
        GOC_DBG("on_handle_close_dispatch: pending write cancel complete for stream=%p\n",
                (void*)stream);

        if (is_goc_stream_ctx(stream->data)) {
            goc_stream_ctx_t* ctx = (goc_stream_ctx_t*)stream->data;
            GOC_DBG("on_handle_close_dispatch: closing pending read stream=%p ch=%p ctx=%p\n",
                    (void*)stream, (void*)ctx->ch, (void*)ctx);
            if (!uv_is_closing((uv_handle_t*)stream)) {
                int stop_rc = uv_read_stop(stream);
                GOC_DBG("on_handle_close_dispatch: uv_read_stop rc=%d stream=%p\n",
                        stop_rc, (void*)stream);
            } else {
                GOC_DBG("on_handle_close_dispatch: stream=%p already closing, skipping uv_read_stop\n",
                        (void*)stream);
            }
            stream->data = ctx->saved_handle_data;
            goc_io_read_t* res = (goc_io_read_t*)goc_malloc(sizeof(goc_io_read_t));
            res->nread = UV_ECANCELED;
            res->buf   = NULL;
            goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
            /* ctx is GC-managed via goc_malloc; do not free it manually. */
            GOC_DBG("on_handle_close_dispatch: restored stream->data after read cleanup stream=%p data=%p\n",
                    (void*)stream, stream->data);
        } else if (is_pending_read_start_marker(stream->data, stream)) {
            goc_read_start_pending_t* pending = (goc_read_start_pending_t*)stream->data;
            pending->canceled = 1;
            stream->data = pending->saved_handle_data;
            GOC_DBG("on_handle_close_dispatch: canceled in-flight pending read start stream=%p ch=%p pending=%p preserved data=%p\n",
                    (void*)stream, (void*)pending->ch, (void*)pending, stream->data);
        } else if (is_server_ctx(stream->data)) {
            goc_server_ctx_t* ctx = (goc_server_ctx_t*)stream->data;
            GOC_DBG("on_handle_close_dispatch: server stream cleanup deferred until close callback ch=%p stream=%p ctx=%p closing=%d\n",
                    (void*)ctx->ch, (void*)stream, (void*)ctx,
                    (int)goc_chan_close_state(ctx->ch));
            /* Preserve stream->data until on_goc_io_handle_close so the server
             * close callback can safely perform accept-channel teardown.
             */
        } else if (is_goc_io_handle_wrapper(stream->data)) {
            goc_io_handle_t* h = (goc_io_handle_t*)stream->data;
            GOC_DBG("on_handle_close_dispatch: preserving wrapper stream->data for target=%p data=%p wrapper=%p state=%d until close callback\n",
                    (void*)stream, stream->data, (void*)h, (int)h->state);
            /* Preserve wrapper handle metadata until on_goc_io_handle_close.  The
             * close callback needs access to this wrapper for cleanup and to
             * maintain ownership invariants. */
        } else {
            goc_io_handle_t* h = goc_io_handle_registry_find((uv_handle_t*)stream);
            if (h) {
                GOC_DBG("on_handle_close_dispatch: recovered wrapper from registry for target=%p wrapper=%p\n",
                        (void*)stream, (void*)h);
                stream->data = h;
            } else {
                GOC_DBG("on_handle_close_dispatch: no wrapper found in registry for target=%p stream->data=%p; clearing data\n",
                        (void*)stream, stream->data);
                stream->data = NULL;
            }
        }
        GOC_DBG("on_handle_close_dispatch: post-cleanup stream=%p data=%p\n",
                (void*)stream, stream->data);
    } else {
        if (d->target->data) {
            GOC_DBG("on_handle_close_dispatch: clearing non-stream handle->data for target=%p type=%s\n",
                    (void*)d->target,
                    uv_handle_type_name(type));
            d->target->data = NULL;
        } else {
            GOC_DBG("on_handle_close_dispatch: target=%p type=%s is not a stream, no pending cleanup needed\n",
                    (void*)d->target,
                    uv_handle_type_name(type));
        }
    }
    GOC_DBG("on_handle_close_dispatch: registering close callback target=%p user_cb=%p ch=%p loop=%p loop_alive=%d\n",
            (void*)d->target, (void*)d->user_cb, (void*)d->ch,
            (void*)d->target->loop,
            d->target->loop ? uv_loop_alive(d->target->loop) : -1);
    register_handle_close_cb_safe(d->target, d->user_cb, d->ch);

    GOC_DBG("on_handle_close_dispatch: scheduling uv_close target=%p type=%s active=%d has_ref=%d closing=%d data=%p\n",
            (void*)d->target,
            uv_handle_type_name(type),
            uv_is_active(d->target),
            uv_has_ref(d->target),
            uv_is_closing(d->target),
            d->target->data);
    GOC_DBG("on_handle_close_dispatch: invoking uv_close target=%p type=%s closing=%d active=%d has_ref=%d data=%p\n",
            (void*)d->target,
            uv_handle_type_name(type),
            uv_is_closing(d->target),
            uv_is_active(d->target),
            uv_has_ref(d->target),
            d->target->data);
    goc_uv_close_log("on_handle_close_dispatch", d->target);
    goc_uv_close_internal(d->target, on_goc_io_handle_close);
    if (d->target && d->target->loop) {
        goc_uv_walk_log(d->target->loop, "on_handle_close_dispatch after uv_close");
    }
    GOC_DBG("on_handle_close_dispatch: uv_close invoked target=%p type=%s closing=%d active=%d has_ref=%d\n",
            (void*)d->target,
            uv_handle_type_name(type),
            uv_is_closing(d->target),
            uv_is_active(d->target),
            uv_has_ref(d->target));
    GOC_DBG("on_handle_close_dispatch: stream cleanup complete for target=%p data=%p\n",
            (void*)d->target, d->target ? d->target->data : NULL);
    GOC_DBG("on_handle_close_dispatch: uv_close scheduled target=%p closing=%d\n",
            (void*)d->target, uv_is_closing(d->target));
    GOC_DBG("on_handle_close_dispatch: exit target=%p type=%s loop=%p closing=%d active=%d has_ref=%d data=%p\n",
            (void*)d->target,
            uv_handle_type_name(type),
            (void*)d->target->loop,
            uv_is_closing(d->target),
            uv_is_active(d->target),
            uv_has_ref(d->target),
            d->target->data);
    goc_uv_close_log("on_handle_close_dispatch exit", d->target);
    goc_uv_callback_exit("on_handle_close_dispatch");
    free(d);
}

static void goc_io_handle_close_internal(uv_handle_t* handle,
                                          uv_close_cb cb,
                                          goc_chan* ch,
                                          bool retry)

{
    if (!handle) {
        GOC_DBG("goc_io_handle_close: called with NULL handle – ignoring\n");
        return;
    }
    uv_loop_t* source_loop = goc_current_worker_loop();
    int source_worker = goc_current_worker_id();
    int deferred = (handle->loop != source_loop) ? 1 : 0;
    GOC_DBG("[uv_close dispatch] source_thread=%llu source_worker=%d source_loop=%p target_loop=%p handle=%p type=%s active=%d closing=%d data=%p deferred=%d\n",
            goc_uv_thread_id(),
            source_worker,
            (void*)source_loop,
            (void*)handle->loop,
            (void*)handle,
            uv_handle_type_name(uv_handle_get_type(handle)),
            uv_is_active(handle),
            uv_is_closing(handle),
            (void*)handle->data,
            deferred);
    goc_uv_close_log("goc_io_handle_close entry", handle);
    GOC_DBG(
            "goc_io_handle_close: dispatch close target=%p loop=%p type=%s active=%d closing=%d has_ref=%d loop_alive=%d data=%p deferred=%d\n",
            (void*)handle,
            (void*)handle->loop,
            uv_handle_type_name(uv_handle_get_type(handle)),
            uv_is_active(handle),
            uv_is_closing(handle),
            uv_has_ref(handle),
            handle->loop ? uv_loop_alive(handle->loop) : -1,
            (void*)handle->data,
            deferred);
    if (!handle->data) {
        GOC_DBG("goc_io_handle_close: begin close with null handle->data target=%p type=%s active=%d closing=%d loop=%p\n",
                (void*)handle,
                uv_handle_type_name(uv_handle_get_type(handle)),
                uv_is_active(handle),
                uv_is_closing(handle),
                (void*)handle->loop);
    } else {
        GOC_DBG("goc_io_handle_close: begin close with data=%p target=%p type=%s active=%d closing=%d loop=%p\n",
                handle->data,
                (void*)handle,
                uv_handle_type_name(uv_handle_get_type(handle)),
                uv_is_active(handle),
                uv_is_closing(handle),
                (void*)handle->loop);
    }
    if (!handle->loop) {
        ABORT("goc_io_handle_close: handle has no loop target=%p type=%s active=%d closing=%d data=%p\n",
              (void*)handle,
              uv_handle_type_name(uv_handle_get_type(handle)),
              handle ? uv_is_active(handle) : -1,
              handle ? uv_is_closing(handle) : -1,
              (void*)handle->data);
    }

    goc_io_handle_t* h = goc_io_handle_from_uv(handle);
    if (!h && handle->type == UV_ASYNC) {
        /* Raw async handles are allowed to be closed directly. They are
         * wrapped internally for close-state tracking and owner checks. */
        h = goc_io_handle_wrap(handle, UV_ASYNC_HANDLE_OWNER);
        GOC_DBG("goc_io_handle_close: auto-wrapping raw async handle %p wrapper=%p\n",
                (void*)handle, (void*)h);
    }
    if (!h) {
        if (uv_is_closing(handle)) {
            GOC_DBG("goc_io_handle_close: already-closing raw uv_handle_t ignored target=%p type=%s active=%d closing=%d data=%p\n",
                    (void*)handle,
                    uv_handle_type_name(uv_handle_get_type(handle)),
                    uv_is_active(handle),
                    uv_is_closing(handle),
                    (void*)handle->data);
            if (cb) {
                cb(handle);
            }
            if (ch) {
                goc_close(ch);
            }
            return;
        }
        if (!handle->data) {
            GOC_DBG("goc_io_handle_close: already-closed handle without wrapper target=%p type=%s\n",
                    (void*)handle,
                    uv_handle_type_name(uv_handle_get_type(handle)));
            if (cb) {
                cb(handle);
            }
            if (ch) {
                goc_close(ch);
            }
            return;
        }
        GOC_DBG("goc_io_handle_close: raw uv_handle_t passed without wrapper, aborting target=%p type=%s active=%d closing=%d data=%p\n",
                (void*)handle,
                uv_handle_type_name(uv_handle_get_type(handle)),
                uv_is_active(handle),
                uv_is_closing(handle),
                (void*)handle->data);
        ABORT("goc_io_handle_close: raw uv_handle_t passed without wrapper target=%p type=%s active=%d closing=%d data=%p\n",
              (void*)handle,
              uv_handle_type_name(uv_handle_get_type(handle)),
              uv_is_active(handle),
              uv_is_closing(handle),
              (void*)handle->data);
    }

    if (!retry) {
        if (!goc_io_handle_claim_close(h)) {
            int state = (int)atomic_load(&h->state);
            GOC_DBG("goc_io_handle_close: duplicate close attempt target=%p wrapper=%p state=%d cb=%p\n",
                    (void*)handle,
                    (void*)h,
                    state,
                    (void*)cb);
            if ((cb || ch) &&
                (goc_deferred_close_contains(handle) || uv_is_closing(handle) ||
                 state == GOC_H_QUEUED || state == GOC_H_CLOSING)) {
                GOC_DBG("goc_io_handle_close: duplicate close request registering callback/channel target=%p wrapper=%p state=%d cb=%p ch=%p\n",
                        (void*)handle,
                        (void*)h,
                        state,
                        (void*)cb,
                        (void*)ch);
                register_handle_close_cb_safe(handle, cb, ch);
            }
            if (state == GOC_H_QUEUED || state == GOC_H_CLOSING) {
                GOC_DBG("goc_io_handle_close: close already in progress target=%p wrapper=%p state=%d cb=%p ch=%p\n",
                        (void*)handle,
                        (void*)h,
                        state,
                        (void*)cb,
                        (void*)ch);
                return;
            }
            if (state == GOC_H_CLOSED) {
                if (cb) {
                    cb(handle);
                }
                if (ch) {
                    goc_close(ch);
                }
                GOC_DBG("goc_io_handle_close: duplicate close already completed target=%p wrapper=%p state=%d cb=%p ch=%p\n",
                        (void*)handle,
                        (void*)h,
                        state,
                        (void*)cb,
                        (void*)ch);
                return;
            }
            ABORT("goc_io_handle_close: invalid wrapper state on duplicate close target=%p wrapper=%p state=%d cb=%p\n",
                  (void*)handle,
                  (void*)h,
                  state,
                  (void*)cb);
        }
    } else {
        int state = (int)atomic_load(&h->state);
        if (state == GOC_H_CLOSED) {
            if (cb && uv_is_closing(handle)) {
                register_handle_close_cb_safe(handle, cb, ch);
            }
            GOC_DBG("goc_io_handle_close: deferred retry close already completed target=%p wrapper=%p state=%d cb=%p\n",
                    (void*)handle,
                    (void*)h,
                    state,
                    (void*)cb);
            return;
        }
        if (state == GOC_H_CLOSING && uv_is_closing(handle)) {
            if (cb) {
                register_handle_close_cb_safe(handle, cb, ch);
            }
            GOC_DBG("goc_io_handle_close: deferred retry close already in progress target=%p wrapper=%p state=%d cb=%p\n",
                    (void*)handle,
                    (void*)h,
                    state,
                    (void*)cb);
            return;
        }
        GOC_DBG("goc_io_handle_close: deferred retry close continuing target=%p wrapper=%p state=%d cb=%p\n",
                (void*)handle,
                (void*)h,
                state,
                (void*)cb);
    }

    if (goc_deferred_close_contains(handle)) {
        if (cb || ch)
            register_handle_close_cb_safe(handle, cb, ch);
        GOC_DBG("goc_io_handle_close: close already deferred target=%p cb=%p ch=%p\n",
                (void*)handle, (void*)cb, (void*)ch);
        return;
    }

    if (handle->loop == g_loop) {
        GOC_DBG("goc_io_handle_close: target is on g_loop g_loop_shutting_down=%d\n",
                goc_loop_is_shutting_down());
        if (handle->data) {
            GOC_DBG("goc_io_handle_close: g_loop target has non-null data=%p type=%s\n",
                    handle->data,
                    uv_handle_type_name(uv_handle_get_type(handle)));
            if (is_server_ctx(handle->data)) {
                goc_server_ctx_t* ctx = (goc_server_ctx_t*)handle->data;
                GOC_DBG("goc_io_handle_close: g_loop target is server ctx=%p accept_ch=%p closing=%d\n",
                        (void*)ctx, (void*)ctx->ch, (int)goc_chan_close_state(ctx->ch));
            } else if (is_pending_read_start_marker(handle->data, (uv_stream_t*)handle)) {
                GOC_DBG("goc_io_handle_close: g_loop target has pending read-start marker=%p\n",
                        handle->data);
            } else if (is_goc_io_handle_wrapper(handle->data)) {
                goc_io_handle_t* h = (goc_io_handle_t*)handle->data;
                GOC_DBG("goc_io_handle_close: g_loop target is wrapper handle=%p owner=%p state=%d\n",
                        (void*)handle, h->owner, (int)h->state);
            } else {
                GOC_DBG("goc_io_handle_close: g_loop target has unknown handle->data=%p\n",
                        handle->data);
            }
        }
    }

    if (goc_deferred_close_contains(handle)) {
        if (cb || ch)
            register_handle_close_cb_safe(handle, cb, ch);
        GOC_DBG("goc_io_handle_close: close already deferred target=%p cb=%p ch=%p\n",
                (void*)handle, (void*)cb, (void*)ch);
        return;
    }

    if (goc_uv_callback_depth > 0 && !uv_is_closing(handle)) {
        if (h && atomic_load(&h->state) == GOC_H_QUEUED) {
            atomic_store(&h->state, GOC_H_CLOSING);
            GOC_DBG("goc_io_handle_close: callback depth defer transitions wrapper=%p uv=%p state QUEUED->CLOSING\n",
                    (void*)h, (void*)handle);
        }
        GOC_DBG("goc_io_handle_close: deferring close because callback_depth=%d target=%p loop=%p type=%s active=%d closing=%d data=%p state=%d\n",
                goc_uv_callback_depth,
                (void*)handle,
                (void*)handle->loop,
                uv_handle_type_name(uv_handle_get_type(handle)),
                uv_is_active(handle),
                uv_is_closing(handle),
                (void*)handle->data,
                h ? (int)atomic_load(&h->state) : -1);
        goc_deferred_close_add(handle, cb, ch);
        return;
    }

    if (h && atomic_load(&h->state) == GOC_H_QUEUED) {
        atomic_store(&h->state, GOC_H_CLOSING);
        GOC_DBG("goc_io_handle_close: close starting transitions wrapper=%p uv=%p state QUEUED->CLOSING\n",
                (void*)h, (void*)handle);
    }
    if (!uv_loop_alive(handle->loop)) {
        GOC_DBG(
                "goc_io_handle_close: target loop %p is dead for handle=%p type=%s active=%d has_ref=%d closing=%d data=%p\n",
                (void*)handle->loop,
                (void*)handle,
                uv_handle_type_name(uv_handle_get_type(handle)),
                uv_is_active(handle),
                uv_has_ref(handle),
                uv_is_closing(handle),
                (void*)handle->data);
        if (uv_is_closing(handle)) {
            if (handle->data && is_goc_io_handle_wrapper(handle->data)) {
                goc_io_handle_t* h = (goc_io_handle_t*)handle->data;
                GOC_DBG(
                        "goc_io_handle_close: dead-loop closing handle %p wrapper cleanup wrapper=%p\n",
                        (void*)handle,
                        (void*)h);
                h->state = GOC_H_CLOSED;
                goc_io_handle_registry_remove(handle);
                handle->data = NULL;
                goc_deferred_wrapper_free_add(h);
            }
            goc_deferred_handle_unreg_add(handle);
            return;
        }
        ABORT("goc_io_handle_close: handle loop %p is dead while handle %p is not closing; invalid close path\n",
              (void*)handle->loop,
              (void*)handle);
    }
    if (uv_is_closing(handle)) {
        GOC_DBG(
                "goc_io_handle_close: target already closing, skipping close target=%p loop=%p active=%d has_ref=%d closing=%d data=%p loop_alive=%d\n",
                (void*)handle,
                (void*)handle->loop,
                uv_is_active(handle),
                uv_has_ref(handle),
                uv_is_closing(handle),
                (void*)handle->data,
                handle->loop ? uv_loop_alive(handle->loop) : -1);
        return;
    }
    goc_io_handle_close_dispatch_t* d = (goc_io_handle_close_dispatch_t*)malloc(
                                         sizeof(goc_io_handle_close_dispatch_t));
    d->target  = handle;
    d->user_cb = cb;
    d->ch      = ch;
    GOC_DBG("goc_io_handle_close: dispatching close target=%p loop=%p type=%s active=%d closing=%d has_ref=%d loop_alive=%d data=%p\n",
            (void*)handle,
            (void*)handle->loop,
            uv_handle_type_name(uv_handle_get_type(handle)),
            uv_is_active(handle),
            uv_is_closing(handle),
            uv_has_ref(handle),
            handle->loop ? uv_loop_alive(handle->loop) : -1,
            handle->data);
    if (retry && handle->loop == g_loop && goc_on_loop_thread()) {
        GOC_DBG("goc_io_handle_close: retry dispatch to g_loop via post_on_loop target=%p\n",
                (void*)handle);
        int rc = post_on_loop_checked(on_handle_close_dispatch, d);
        if (rc < 0) {
            GOC_DBG("goc_io_handle_close: post_on_loop_checked failed on retry rc=%d target=%p, falling back to direct dispatch\n",
                    rc, (void*)handle);
            on_handle_close_dispatch(d);
        }
    } else {
        dispatch_on_handle_loop(handle->loop, on_handle_close_dispatch, d);
    }
    GOC_DBG("goc_io_handle_close: dispatch completed target=%p loop=%p\n",
            (void*)handle,
            (void*)handle->loop);
}

goc_chan* goc_io_handle_close(uv_handle_t* handle)
{
    goc_chan* ch = goc_chan_make(1);
    if (!handle) {
        goc_put_cb(ch, SCALAR(UV_EINVAL), goc_close_cb, ch);
        return ch;
    }
    goc_io_handle_close_internal(handle, NULL, ch, false);
    return ch;
}



/* =========================================================================
 * TCP bind / server / socket options
 * ====================================================================== */

/* Generic dispatch: one handle pointer + one int result (SCALAR). */
typedef struct {
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

static void on_simple_dispatch(void* arg)
{
    goc_simple_dispatch_t* d = (goc_simple_dispatch_t*)arg;
    int rc = 0;
    switch (d->kind) {
        /* TCP */
        case 0:  rc = uv_tcp_bind((uv_tcp_t*)d->handle,
                                  (const struct sockaddr*)&d->addr, 0); break;
        case 1:  rc = uv_tcp_keepalive((uv_tcp_t*)d->handle, d->i1, d->u1); break;
        case 2:  rc = uv_tcp_nodelay((uv_tcp_t*)d->handle, d->i1); break;
        case 3:  rc = uv_tcp_simultaneous_accepts((uv_tcp_t*)d->handle, d->i1); break;
        case 4: {
            uv_os_fd_t fd;
            GOC_DBG("on_simple_dispatch: tcp_reuseaddr handle=%p loop=%p\n",
                    (void*)d->handle,
                    (void*)(d->handle ? ((uv_handle_t*)d->handle)->loop : NULL));
            rc = uv_fileno((const uv_handle_t*)d->handle, &fd);
            if (rc == 0) {
                GOC_DBG("on_simple_dispatch: tcp_reuseaddr uv_fileno handle=%p fd=%d\n",
                        (void*)d->handle,
                        (int)fd);
                int opt = d->i1;
#ifdef _WIN32
                if (setsockopt((SOCKET)fd, SOL_SOCKET, SO_REUSEADDR,
                               (const char*)&opt, sizeof(opt)) != 0) {
                    int saved_errno = WSAGetLastError();
                    GOC_DBG("on_simple_dispatch: tcp_reuseaddr setsockopt failed handle=%p fd=%d errno=%d\n",
                            (void*)d->handle,
                            (int)fd,
                            saved_errno);
                    rc = -saved_errno;
                } else {
                    GOC_DBG("on_simple_dispatch: tcp_reuseaddr setsockopt OK handle=%p fd=%d\n",
                            (void*)d->handle,
                            (int)fd);
                }
#else
                if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                               (const char*)&opt, sizeof(opt)) != 0) {
                    int saved_errno = errno;
                    GOC_DBG("on_simple_dispatch: tcp_reuseaddr setsockopt failed handle=%p fd=%d errno=%d\n",
                            (void*)d->handle,
                            (int)fd,
                            saved_errno);
                    rc = -saved_errno;
                } else {
                    GOC_DBG("on_simple_dispatch: tcp_reuseaddr setsockopt OK handle=%p fd=%d\n",
                            (void*)d->handle,
                            (int)fd);
                }
#endif
            } else {
                GOC_DBG("on_simple_dispatch: tcp_reuseaddr uv_fileno failed handle=%p rc=%d\n",
                        (void*)d->handle,
                        rc);
            }
            break;
        }
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
    goc_put_cb(d->ch, SCALAR(rc), goc_close_cb, d->ch);
    free(d);
}

typedef struct {
    void*      handle;
    uv_os_fd_t fd;
    goc_chan*  ch;
} goc_io_tcp_open_dispatch_t;

static void on_tcp_open_dispatch(void* arg)
{
    goc_io_tcp_open_dispatch_t* d = (goc_io_tcp_open_dispatch_t*)arg;
    int rc = uv_tcp_open((uv_tcp_t*)d->handle,
                         (uv_os_sock_t)(uintptr_t)d->fd);
    goc_put_cb(d->ch, SCALAR(rc), goc_close_cb, d->ch);
    free(d);
}

/* Dispatch uv_tcp_open; delivers goc_box_int(status). */
goc_chan* goc_io_tcp_open(uv_tcp_t* handle, uv_os_fd_t fd)
{
    goc_chan* dch = goc_chan_make(1);
    goc_io_tcp_open_dispatch_t* d = (goc_io_tcp_open_dispatch_t*)malloc(
        sizeof(goc_io_tcp_open_dispatch_t));
    d->handle = handle;
    d->fd = fd;
    d->ch = dch;
    dispatch_on_handle_loop(((uv_handle_t*)handle)->loop, on_tcp_open_dispatch, d);
    return dch;
}

static goc_chan* simple_dispatch(void* handle, int kind,
                                 const struct sockaddr* addr,
                                 int i1, unsigned u1,
                                 const char* s1, const char* s2,
                                 const char* s3,
                                 uv_membership membership)
{
    goc_chan*               ch = goc_chan_make(1);
    goc_simple_dispatch_t*  d  = (goc_simple_dispatch_t*)malloc(
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
    dispatch_on_handle_loop(((uv_handle_t*)handle)->loop, on_simple_dispatch, d);
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

goc_chan* goc_io_tcp_reuseaddr(uv_tcp_t* handle)
{
    return simple_dispatch(handle, 4, NULL, 1, 0, NULL, NULL, NULL, 0);
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
    goc_simple_dispatch_t*  d  = (goc_simple_dispatch_t*)malloc(
                                     sizeof(goc_simple_dispatch_t));
    d->handle = NULL;
    d->kind   = 31;
    d->ch     = ch;
    d->i1     = pid;
    d->u1     = (unsigned)signum;
    post_on_loop(on_simple_dispatch, d);
    return ch;
}

/* =========================================================================
 * TCP server / Pipe server
 * ====================================================================== */

/* Called by loop_process_pending_put when an accepted-connection put is
 * dropped because the accept channel was closed (server shutting down).
 * Closes the server-side handle so the client receives FIN/RST instead of
 * hanging indefinitely waiting for data that will never arrive. */

// Cleanup callback for accepted TCP connection
static void on_accepted_tcp_dropped(goc_chan* _ch, void* _val, goc_status_t ok, void* ud)
{
    goc_io_handle_t* h = (goc_io_handle_t*)ud;
    uv_tcp_t* conn = h ? (uv_tcp_t*)h->uv : NULL;
    GOC_DBG("on_accepted_tcp_dropped: ok=%d conn=%p data=%p active=%d closing=%d owner=%p\n",
            (int)ok, (void*)conn, conn ? conn->data : NULL,
            conn ? uv_is_active((uv_handle_t*)conn) : -1,
            conn ? uv_is_closing((uv_handle_t*)conn) : -1,
            h ? h->owner : NULL);
    if (ok != GOC_OK && h && goc_io_handle_is_open(h) && !uv_is_closing(h->uv)) {
        GOC_DBG("on_accepted_tcp_dropped: dropping accepted conn=%p active=%d closing=%d owner=%p\n",
                (void*)conn,
                conn ? uv_is_active((uv_handle_t*)conn) : -1,
                conn ? uv_is_closing((uv_handle_t*)conn) : -1,
                h->owner);
        goc_io_handle_request_close(h, NULL);
    }
}

// Cleanup callback for accepted pipe connection
static void on_accepted_pipe_dropped(goc_chan* _ch, void* _val, goc_status_t ok, void* ud)
{
    goc_io_handle_t* h = (goc_io_handle_t*)ud;
    uv_pipe_t* conn = h ? (uv_pipe_t*)h->uv : NULL;
    GOC_DBG("on_accepted_pipe_dropped: ok=%d conn=%p data=%p active=%d closing=%d owner=%p\n",
            (int)ok, (void*)conn, conn ? conn->data : NULL,
            conn ? uv_is_active((uv_handle_t*)conn) : -1,
            conn ? uv_is_closing((uv_handle_t*)conn) : -1,
            h ? h->owner : NULL);
    if (ok != GOC_OK && h && goc_io_handle_is_open(h) && !uv_is_closing(h->uv)) {
        GOC_DBG("on_accepted_pipe_dropped: dropping accepted conn=%p active=%d closing=%d owner=%p\n",
                (void*)conn,
                conn ? uv_is_active((uv_handle_t*)conn) : -1,
                conn ? uv_is_closing((uv_handle_t*)conn) : -1,
                h->owner);
        goc_io_handle_request_close(h, NULL);
    }
}

static void close_chan_if_not_closing(goc_chan* ch)
{
    if (ch == NULL) {
        GOC_DBG("close_chan_if_not_closing: ch=NULL, skipping\n");
        return;
    }
    GOC_DBG("close_chan_if_not_closing: ch=%p\n", (void*)ch);
    goc_close(ch);
}

static void drop_buffered_accept_connections(goc_chan* ch)
{
    void* item = NULL;

    uv_mutex_lock(ch->lock);
    while (chan_take_from_buffer(ch, &item)) {
        uv_mutex_unlock(ch->lock);
        if (item != NULL) {
            uv_handle_t* handle = (uv_handle_t*)item;
            goc_io_handle_t* h = goc_io_handle_registry_find(handle);
            if (h && goc_io_handle_is_owned_by(h, ch) && goc_io_handle_is_open(h) && !uv_is_closing(handle)) {
                goc_uv_close_log("drop_buffered_accept_connections close", handle);
                goc_io_handle_request_close(h, NULL);
            } else if (h) {
                GOC_DBG("drop_buffered_accept_connections: handle=%p wrapper=%p owner=%p not owned by ch=%p or already closed\n",
                        (void*)handle, (void*)h, (void*)h->owner, (void*)ch);
            } else {
                ABORT("%s: raw uv_handle_t %p found in accept buffer; raw-handle accept buffering is no longer supported\n",
                      __func__, (void*)handle);
            }
        }
        uv_mutex_lock(ch->lock);
    }
    uv_mutex_unlock(ch->lock);
}

static void on_server_connection(uv_stream_t* server, int status)
{
    goc_uv_callback_enter("on_server_connection");
    int worker_id = goc_current_worker_id();
    GOC_DBG(
            "on_server_connection: server=%p status=%d server_data=%p loop=%p current_worker=%d\n",
            (void*)server, status, (void*)server->data, (void*)server->loop, worker_id);
    goc_server_ctx_t* ctx = (goc_server_ctx_t*)server->data;
    if (!ctx) {
        GOC_DBG("on_server_connection: server=%p data=NULL, ignoring late callback after close\n",
                (void*)server);
        return;
    }
    if (status < 0) {
        /* Error: close channel. */
        GOC_DBG(
                "on_server_connection: status<0 closing accept channel ch=%p server=%p status=%d\n",
                (void*)ctx->ch, (void*)server, status);
        drop_buffered_accept_connections(ctx->ch);
        GOC_DBG("on_server_connection: status<0 close accept channel ch=%p caller=status<0\n",
                (void*)ctx->ch);
        close_chan_if_not_closing(ctx->ch);
        server->data = NULL;
        return;
    }

    if (ctx->is_tcp) {
        uv_tcp_t* client = (uv_tcp_t*)goc_malloc(sizeof(uv_tcp_t));
        int rc = uv_tcp_init(server->loop, client);
        goc_uv_init_log("on_server_connection uv_tcp_init", rc, server->loop, (uv_handle_t*)client);
        if (rc < 0) {
            GOC_DBG("on_server_connection: uv_tcp_init failed rc=%d server=%p\n", rc, (void*)server);
            drop_buffered_accept_connections(ctx->ch);
            GOC_DBG("on_server_connection: uv_tcp_init failed close accept channel ch=%p caller=uv_tcp_init_failed\n",
                    (void*)ctx->ch);
            close_chan_if_not_closing(ctx->ch);
            server->data = NULL;
            return;
        }
        client->data = NULL;
        goc_io_handle_t* client_handle = goc_io_handle_wrap((uv_handle_t*)client, ctx->ch);
        client->data = client_handle;
        GOC_DBG("on_server_connection: initialized accepted tcp client=%p data=%p owner=%p\n",
                (void*)client, client->data, (void*)client_handle->owner);
        gc_handle_register(client);
        rc = uv_accept(server, (uv_stream_t*)client);
        GOC_DBG("on_server_connection: uv_accept rc=%d client=%p ch=%p ch_closing=%d\n",
                rc, (void*)client, (void*)ctx->ch, (int)goc_chan_close_state(ctx->ch));
        if (rc < 0) {
            goc_uv_close_log("on_server_connection accepted tcp client close", (uv_handle_t*)client);
            goc_uv_handle_log("on_server_connection accepted tcp client close", (uv_handle_t*)client);
            goc_io_handle_request_close(client_handle, NULL);
            goc_uv_callback_exit("on_server_connection");
            return;
        }
        if (!goc_chan_is_open(ctx->ch)) {
            GOC_DBG(
                    "on_server_connection: accept ch already closing, dropping accepted tcp client=%p ch=%p active=%d closing=%d reason=accept_ch_closing\n",
                    (void*)client,
                    (void*)ctx->ch,
                    uv_is_active((uv_handle_t*)client),
                    uv_is_closing((uv_handle_t*)client));
            goc_uv_close_log("on_server_connection accepted tcp client close on channel closing", (uv_handle_t*)client);
            goc_uv_handle_log("on_server_connection accepted tcp client close on channel closing", (uv_handle_t*)client);
            goc_io_handle_request_close(client_handle, NULL);
            goc_uv_callback_exit("on_server_connection");
            return;
        }
        GOC_DBG(
                "on_server_connection: goc_put_cb client=%p ch=%p active=%d closing=%d\n",
                (void*)client,
                (void*)ctx->ch,
                uv_is_active((uv_handle_t*)client),
                uv_is_closing((uv_handle_t*)client));
        goc_put_cb(ctx->ch, client, on_accepted_tcp_dropped, client_handle);
        GOC_DBG(
                "on_server_connection: accepted client enqueued ch=%p client=%p\n",
                (void*)ctx->ch,
                (void*)client);
    } else {
        uv_pipe_t* client = (uv_pipe_t*)goc_malloc(sizeof(uv_pipe_t));
        int rc = uv_pipe_init(server->loop, client, 0);
        goc_uv_init_log("on_server_connection uv_pipe_init", rc, server->loop, (uv_handle_t*)client);
        if (rc < 0) {
            GOC_DBG("on_server_connection: uv_pipe_init failed close accept channel ch=%p caller=uv_pipe_init_failed\n",
                    (void*)ctx->ch);
            close_chan_if_not_closing(ctx->ch);
            server->data = NULL;
            return;
        }
        client->data = NULL;
        goc_io_handle_t* client_handle = goc_io_handle_wrap((uv_handle_t*)client, ctx->ch);
        client->data = client_handle;
        GOC_DBG("on_server_connection: initialized accepted pipe client=%p data=%p owner=%p\n",
                (void*)client, client->data, (void*)client_handle->owner);
        gc_handle_register(client);
        rc = uv_accept(server, (uv_stream_t*)client);
        if (rc < 0) {
            GOC_DBG("on_server_connection: uv_accept failed, dropping accepted pipe client close ch=%p reason=uv_accept_failed\n",
                    (void*)ctx->ch);
            goc_uv_close_log("on_server_connection accepted pipe client close", (uv_handle_t*)client);
            goc_uv_handle_log("on_server_connection accepted pipe client close", (uv_handle_t*)client);
            goc_io_handle_request_close(client_handle, NULL);
            goc_uv_callback_exit("on_server_connection");
            return;
        }
        if (!goc_chan_is_open(ctx->ch)) {
            GOC_DBG(
                    "on_server_connection: accept ch already closing, dropping accepted pipe client=%p ch=%p active=%d closing=%d reason=accept_ch_closing\n",
                    (void*)client,
                    (void*)ctx->ch,
                    uv_is_active((uv_handle_t*)client),
                    uv_is_closing((uv_handle_t*)client));
            goc_uv_close_log("on_server_connection accepted pipe client close on channel closing", (uv_handle_t*)client);
            goc_uv_handle_log("on_server_connection accepted pipe client close on channel closing", (uv_handle_t*)client);
            goc_io_handle_request_close(client_handle, NULL);
            goc_uv_callback_exit("on_server_connection");
            return;
        }
        goc_put_cb(ctx->ch, client, on_accepted_pipe_dropped, client_handle);
    }
    goc_uv_callback_exit("on_server_connection");
}

typedef struct {
    uv_stream_t*  server;
    int           backlog;
    goc_chan*     ch;
    int           is_tcp;
    goc_chan*     ready_ch; /* optional: delivers goc_box_int(rc) when uv_listen returns */
} goc_server_make_dispatch_t;

static void on_server_make_dispatch(void* arg)
{
    goc_server_make_dispatch_t* d = (goc_server_make_dispatch_t*)arg;
    GOC_DBG(
            "on_server_make_dispatch: server=%p loop=%p backlog=%d ch=%p ready_ch=%p\n",
            (void*)d->server, (void*)d->server->loop, d->backlog,
            (void*)d->ch, (void*)d->ready_ch);

    goc_server_ctx_t* ctx = (goc_server_ctx_t*)goc_malloc(sizeof(goc_server_ctx_t));
    ctx->magic   = GOC_SERVER_CTX_MAGIC;
    ctx->ch      = d->ch;
    ctx->is_tcp  = d->is_tcp;
    d->server->data = ctx;

    int rc = uv_listen(d->server, d->backlog, on_server_connection);
    GOC_DBG("on_server_make_dispatch: uv_listen rc=%d server=%p backlog=%d\n",
            rc, (void*)d->server, d->backlog);
    if (rc < 0) {
        /* Deliver error and close channel. */
        GOC_DBG(
                "on_server_make_dispatch: uv_listen failed, closing accept channel ch=%p server=%p\n",
                (void*)d->ch, (void*)d->server);
        drop_buffered_accept_connections(d->ch);
        GOC_DBG("on_server_make_dispatch: uv_listen failed close accept channel ch=%p caller=uv_listen_failed\n",
                (void*)d->ch);
        close_chan_if_not_closing(d->ch);
        d->server->data = NULL;
    }

    /* Signal ready channel (if any) with the listen result. */
    if (d->ready_ch) {
        GOC_DBG(
                "on_server_make_dispatch: signalling ready_ch=%p rc=%d server=%p\n",
                (void*)d->ready_ch, rc, (void*)d->server);
        goc_put_cb(d->ready_ch, goc_box_int(rc), goc_close_cb, d->ready_ch);
    }
}

goc_chan* goc_io_tcp_server_make(uv_tcp_t* handle, int backlog, goc_chan* ready_ch)
{
    /* Match the accept channel buffer to the socket backlog so heavy connect
     * churn does not stall the server on a tiny channel queue. */
    size_t buf_size = backlog > 0 ? (size_t)backlog : 16;
    goc_chan*                    ch = goc_chan_make(buf_size);
    goc_chan_set_debug_tag(ch, "goc_tcp_server_accept_ch");
    goc_server_make_dispatch_t*  d  = (goc_server_make_dispatch_t*)goc_malloc(
                                          sizeof(goc_server_make_dispatch_t));
    d->server   = (uv_stream_t*)handle;
    d->backlog  = backlog;
    d->ch       = ch;
    d->is_tcp   = 1;
    d->ready_ch = ready_ch;
    GOC_DBG(
            "goc_io_tcp_server_make: handle=%p backlog=%d ch=%p ready_ch=%p loop=%p\n",
            (void*)handle, backlog, (void*)ch, (void*)ready_ch, (void*)handle->loop);
    dispatch_on_handle_loop(((uv_handle_t*)handle)->loop, on_server_make_dispatch, d);
    return ch;
}

goc_chan* goc_io_pipe_server_make(uv_pipe_t* handle, int backlog, goc_chan* ready_ch)
{
    size_t buf_size = backlog > 0 ? (size_t)backlog : 16;
    goc_chan*                    ch = goc_chan_make(buf_size);
    goc_server_make_dispatch_t*  d  = (goc_server_make_dispatch_t*)goc_malloc(
                                          sizeof(goc_server_make_dispatch_t));
    d->server   = (uv_stream_t*)handle;
    d->backlog  = backlog;
    d->ch       = ch;
    d->is_tcp   = 0;
    d->ready_ch = ready_ch;
    dispatch_on_handle_loop(((uv_handle_t*)handle)->loop, on_server_make_dispatch, d);
    return ch;
}

/* =========================================================================
 * TTY get winsize
 * ====================================================================== */

typedef struct {
    uv_tty_t*   handle;
    goc_chan*   ch;
} goc_tty_winsize_dispatch_t;

static void on_tty_winsize_dispatch(void* arg)
{
    goc_tty_winsize_dispatch_t* d = (goc_tty_winsize_dispatch_t*)arg;
    goc_io_tty_winsize_t* res = (goc_io_tty_winsize_t*)goc_malloc(
                                    sizeof(goc_io_tty_winsize_t));
    int w = 0, ht = 0;
    int rc = uv_tty_get_winsize(d->handle, &w, &ht);
    res->ok     = (rc == 0) ? GOC_IO_OK : GOC_IO_ERR;
    res->width  = w;
    res->height = ht;
    goc_put_cb(d->ch, res, goc_close_cb, d->ch);
}

goc_chan* goc_io_tty_get_winsize(uv_tty_t* handle)
{
    goc_chan*                    ch = goc_chan_make(1);
    goc_tty_winsize_dispatch_t*  d  = (goc_tty_winsize_dispatch_t*)goc_malloc(
                                          sizeof(goc_tty_winsize_dispatch_t));
    d->handle = handle;
    d->ch     = ch;
    dispatch_on_handle_loop(((uv_handle_t*)handle)->loop, on_tty_winsize_dispatch, d);
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
    uv_signal_t*  handle;
    int           signum;
    goc_chan*     ch;
} goc_signal_start_dispatch_t;

static void on_signal_start_dispatch(void* arg)
{
    goc_signal_start_dispatch_t* d = (goc_signal_start_dispatch_t*)arg;

    goc_signal_ctx_t* ctx = (goc_signal_ctx_t*)goc_malloc(sizeof(goc_signal_ctx_t));
    ctx->ch     = d->ch;
    ctx->signum = d->signum;
    d->handle->data = ctx;

    int rc = uv_signal_start(d->handle, on_signal_cb, d->signum);
    if (rc < 0) {
        goc_close(d->ch);
        d->handle->data = NULL;
    }
}

goc_chan* goc_io_signal_start(uv_signal_t* handle, int signum)
{
    goc_chan*                    ch = goc_chan_make(16);
    goc_signal_start_dispatch_t* d  = (goc_signal_start_dispatch_t*)goc_malloc(
                                          sizeof(goc_signal_start_dispatch_t));
    d->handle = handle;
    d->signum = signum;
    d->ch     = ch;
    dispatch_on_handle_loop(((uv_handle_t*)handle)->loop, on_signal_start_dispatch, d);
    return ch;
}

typedef struct {
    uv_signal_t* handle;
} goc_signal_stop_dispatch_t;

static void on_signal_stop_dispatch(void* arg)
{
    goc_signal_stop_dispatch_t* d = (goc_signal_stop_dispatch_t*)arg;
    uv_signal_stop(d->handle);
    if (d->handle->data) {
        goc_signal_ctx_t* ctx = (goc_signal_ctx_t*)d->handle->data;
        goc_close(ctx->ch);
        d->handle->data = NULL;
    }
}

int goc_io_signal_stop(uv_signal_t* handle)
{
    goc_signal_stop_dispatch_t* d = (goc_signal_stop_dispatch_t*)goc_malloc(
                                        sizeof(goc_signal_stop_dispatch_t));
    d->handle = handle;
    dispatch_on_handle_loop(((uv_handle_t*)handle)->loop, on_signal_stop_dispatch, d);
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
    uv_fs_event_t*  handle;
    char*           path;
    unsigned        flags;
    goc_chan*       ch;
} goc_fs_event_start_dispatch_t;

static void on_fs_event_start_dispatch(void* arg)
{
    goc_fs_event_start_dispatch_t* d = (goc_fs_event_start_dispatch_t*)arg;

    goc_fs_event_ctx_t* ctx = (goc_fs_event_ctx_t*)goc_malloc(sizeof(goc_fs_event_ctx_t));
    ctx->ch = d->ch;
    d->handle->data = ctx;

    int rc = uv_fs_event_start(d->handle, on_fs_event_cb, d->path, d->flags);
    if (rc < 0) {
        goc_close(d->ch);
        d->handle->data = NULL;
    }
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
    post_on_loop(on_fs_event_start_dispatch, d);
    return ch;
}

typedef struct {
    uv_fs_event_t* handle;
} goc_fs_event_stop_dispatch_t;

static void on_fs_event_stop_dispatch(void* arg)
{
    goc_fs_event_stop_dispatch_t* d = (goc_fs_event_stop_dispatch_t*)arg;
    uv_fs_event_stop(d->handle);
    if (d->handle->data) {
        goc_fs_event_ctx_t* ctx = (goc_fs_event_ctx_t*)d->handle->data;
        goc_close(ctx->ch);
        d->handle->data = NULL;
    }
}

int goc_io_fs_event_stop(uv_fs_event_t* handle)
{
    goc_fs_event_stop_dispatch_t* d = (goc_fs_event_stop_dispatch_t*)goc_malloc(
                                          sizeof(goc_fs_event_stop_dispatch_t));
    d->handle = handle;
    post_on_loop(on_fs_event_stop_dispatch, d);
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
    uv_fs_poll_t*  handle;
    char*          path;
    unsigned       interval_ms;
    goc_chan*      ch;
} goc_fs_poll_start_dispatch_t;

static void on_fs_poll_start_dispatch(void* arg)
{
    goc_fs_poll_start_dispatch_t* d = (goc_fs_poll_start_dispatch_t*)arg;

    goc_fs_poll_ctx_t* ctx = (goc_fs_poll_ctx_t*)goc_malloc(sizeof(goc_fs_poll_ctx_t));
    ctx->ch = d->ch;
    d->handle->data = ctx;

    int rc = uv_fs_poll_start(d->handle, on_fs_poll_cb, d->path, d->interval_ms);
    if (rc < 0) {
        goc_close(d->ch);
        d->handle->data = NULL;
    }
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
    post_on_loop(on_fs_poll_start_dispatch, d);
    return ch;
}

typedef struct {
    uv_fs_poll_t* handle;
} goc_fs_poll_stop_dispatch_t;

static void on_fs_poll_stop_dispatch(void* arg)
{
    goc_fs_poll_stop_dispatch_t* d = (goc_fs_poll_stop_dispatch_t*)arg;
    uv_fs_poll_stop(d->handle);
    if (d->handle->data) {
        goc_fs_poll_ctx_t* ctx = (goc_fs_poll_ctx_t*)d->handle->data;
        goc_close(ctx->ch);
        d->handle->data = NULL;
    }
}

int goc_io_fs_poll_stop(uv_fs_poll_t* handle)
{
    goc_fs_poll_stop_dispatch_t* d = (goc_fs_poll_stop_dispatch_t*)goc_malloc(
                                         sizeof(goc_fs_poll_stop_dispatch_t));
    d->handle = handle;
    post_on_loop(on_fs_poll_stop_dispatch, d);
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
    goc_put_cb(ctx->ch, SCALAR(result), goc_close_cb, ctx->ch);
}

/* Macro to define trivial scalar FS wrappers. */
#define DEF_FS_SCALAR(fname, uv_call)                                        \
    goc_chan* fname {                                                          \
        goc_chan*             ch  = goc_chan_make(1);                          \
        goc_fs_scalar_ctx_t* ctx = (goc_fs_scalar_ctx_t*)goc_malloc(          \
                                       sizeof(goc_fs_scalar_ctx_t));           \
        ctx->ch = ch;                                                          \
        int rc = uv_call;                                                      \
        if (rc < 0) { goc_put_cb(ch, SCALAR(rc), goc_close_cb, ch); return ch; } \
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
    goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
}

goc_chan* goc_io_fs_lstat(const char* path)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_lstat(goc_worker_or_default_loop(), &ctx->req, path, on_fs_lstat);
    if (rc < 0) {
        goc_io_fs_stat_t* res = (goc_io_fs_stat_t*)goc_malloc(sizeof(goc_io_fs_stat_t));
        res->ok = GOC_IO_ERR;
        goc_put_cb(ch, res, goc_close_cb, ch);
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
    goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
}

goc_chan* goc_io_fs_fstat(uv_file file)
{
    goc_chan*     ch  = goc_chan_make(1);
    goc_fs_ctx_t* ctx = (goc_fs_ctx_t*)goc_malloc(sizeof(goc_fs_ctx_t));
    ctx->ch = ch;
    int rc = uv_fs_fstat(goc_worker_or_default_loop(), &ctx->req, file, on_fs_fstat);
    if (rc < 0) {
        goc_io_fs_stat_t* res = (goc_io_fs_stat_t*)goc_malloc(sizeof(goc_io_fs_stat_t));
        res->ok = GOC_IO_ERR;
        goc_put_cb(ch, res, goc_close_cb, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}

/* ftruncate */
DEF_FS_SCALAR(goc_io_fs_ftruncate(uv_file file, int64_t offset),
              uv_fs_ftruncate(goc_worker_or_default_loop(), &ctx->req, file, offset, on_fs_scalar))

/* access */
DEF_FS_SCALAR(goc_io_fs_access(const char* path, int mode),
              uv_fs_access(goc_worker_or_default_loop(), &ctx->req, path, mode, on_fs_scalar))

/* chmod */
DEF_FS_SCALAR(goc_io_fs_chmod(const char* path, int mode),
              uv_fs_chmod(goc_worker_or_default_loop(), &ctx->req, path, mode, on_fs_scalar))

/* fchmod */
DEF_FS_SCALAR(goc_io_fs_fchmod(uv_file file, int mode),
              uv_fs_fchmod(goc_worker_or_default_loop(), &ctx->req, file, mode, on_fs_scalar))

/* chown */
DEF_FS_SCALAR(goc_io_fs_chown(const char* path, uv_uid_t uid, uv_gid_t gid),
              uv_fs_chown(goc_worker_or_default_loop(), &ctx->req, path, uid, gid, on_fs_scalar))

/* fchown */
DEF_FS_SCALAR(goc_io_fs_fchown(uv_file file, uv_uid_t uid, uv_gid_t gid),
              uv_fs_fchown(goc_worker_or_default_loop(), &ctx->req, file, uid, gid, on_fs_scalar))

/* utime */
DEF_FS_SCALAR(goc_io_fs_utime(const char* path, double atime, double mtime),
              uv_fs_utime(goc_worker_or_default_loop(), &ctx->req, path, atime, mtime, on_fs_scalar))

/* futime */
DEF_FS_SCALAR(goc_io_fs_futime(uv_file file, double atime, double mtime),
              uv_fs_futime(goc_worker_or_default_loop(), &ctx->req, file, atime, mtime, on_fs_scalar))

/* lutime */
DEF_FS_SCALAR(goc_io_fs_lutime(const char* path, double atime, double mtime),
              uv_fs_lutime(goc_worker_or_default_loop(), &ctx->req, path, atime, mtime, on_fs_scalar))

/* mkdir */
DEF_FS_SCALAR(goc_io_fs_mkdir(const char* path, int mode),
              uv_fs_mkdir(goc_worker_or_default_loop(), &ctx->req, path, mode, on_fs_scalar))

/* rmdir */
DEF_FS_SCALAR(goc_io_fs_rmdir(const char* path),
              uv_fs_rmdir(goc_worker_or_default_loop(), &ctx->req, path, on_fs_scalar))

/* copyfile */
DEF_FS_SCALAR(goc_io_fs_copyfile(const char* src, const char* dst, int flags),
              uv_fs_copyfile(goc_worker_or_default_loop(), &ctx->req, src, dst, flags, on_fs_scalar))

/* link */
DEF_FS_SCALAR(goc_io_fs_link(const char* path, const char* new_path),
              uv_fs_link(goc_worker_or_default_loop(), &ctx->req, path, new_path, on_fs_scalar))

/* symlink */
DEF_FS_SCALAR(goc_io_fs_symlink(const char* path, const char* new_path, int flags),
              uv_fs_symlink(goc_worker_or_default_loop(), &ctx->req, path, new_path, flags, on_fs_scalar))

/* fsync */
DEF_FS_SCALAR(goc_io_fs_fsync(uv_file file),
              uv_fs_fsync(goc_worker_or_default_loop(), &ctx->req, file, on_fs_scalar))

/* fdatasync */
DEF_FS_SCALAR(goc_io_fs_fdatasync(uv_file file),
              uv_fs_fdatasync(goc_worker_or_default_loop(), &ctx->req, file, on_fs_scalar))

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
                goc_put_cb(ctx->ch, SCALAR(result), goc_close_cb, ctx->ch);
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
            goc_put_cb(ctx->ch, SCALAR(final_rc), goc_close_cb, ctx->ch);
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
        goc_put_cb(ch, SCALAR(rc), goc_close_cb, ch);
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
        goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
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
    goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
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
        goc_put_cb(ch, res, goc_close_cb, ch);
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
    goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
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
        goc_put_cb(ch, res, goc_close_cb, ch);
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
    goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
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
        goc_put_cb(ch, res, goc_close_cb, ch);
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
    goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
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
        goc_put_cb(ch, res, goc_close_cb, ch);
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
        goc_put_cb(ch, res, goc_close_cb, ch);
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
    goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
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
        goc_put_cb(ch, res, goc_close_cb, ch);
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
                goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
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
            goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
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
        goc_put_cb(ch, res, goc_close_cb, ch);
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
                goc_put_cb(ctx->ch, SCALAR((int)req->result), goc_close_cb, ctx->ch);
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
            goc_put_cb(ctx->ch, SCALAR(ctx->error), goc_close_cb, ctx->ch);
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
        goc_put_cb(ch, SCALAR(rc), goc_close_cb, ch);
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
                goc_put_cb(ctx->ch, chunk, goc_close_cb, ctx->ch);
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
                goc_put_cb(ctx->ch, chunk, goc_close_cb, ctx->ch);
            } else if (req->result == 0) {
                /* EOF: close file and close channel. */
                uv_fs_t close_req;
                uv_fs_close(NULL, &close_req, ctx->fd, NULL);
                uv_fs_req_cleanup(&close_req);
                gc_handle_unregister(ctx);
                close_chan_if_not_closing(ctx->ch);
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
        goc_put_cb(ch, chunk, goc_close_cb, ch);
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
    goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
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
        goc_put_cb(ch, res, goc_close_cb, ch);
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
    goc_put_cb(ctx->ch, SCALAR(result), goc_close_cb, ctx->ch);
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
        goc_put_cb(ch, SCALAR(rc), goc_close_cb, ch);
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
            goc_put_cb(ctx->ch, SCALAR(ctx->error), goc_close_cb, ctx->ch);
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
        goc_put_cb(ch, SCALAR(rc), goc_close_cb, ch);
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
    goc_put_cb(ctx->ch, res, goc_close_cb, ctx->ch);
}

goc_chan* goc_io_random(size_t n, unsigned flags)
{
    goc_chan*          ch  = goc_chan_make(1);
    goc_random_ctx_t*  ctx = (goc_random_ctx_t*)goc_malloc(sizeof(goc_random_ctx_t));
    ctx->ch  = ch;
    ctx->n   = n;
    ctx->raw = (char*)goc_malloc(n + 1);

    int rc = uv_random(goc_worker_or_default_loop(), &ctx->req, ctx->raw, n, flags, on_random);
    if (rc < 0) {
        goc_io_random_t* res = (goc_io_random_t*)goc_malloc(sizeof(goc_io_random_t));
        res->ok   = GOC_IO_ERR;
        res->data = NULL;
        goc_put_cb(ch, res, goc_close_cb, ch);
        return ch;
    }
    gc_handle_register(ctx);
    return ch;
}
/* -------------------------------------------------------------------------
 * goc_io_fs_open
 * ---------------------------------------------------------------------- */

/**
 * Opens a file asynchronously.
 *
 * @param path Path to the file to open.
 * @param flags File open flags.
 * @param mode File mode.
 * @return A channel delivering the result of the operation.
 *         On error, the channel delivers a scalar error code.
 */

/* -------------------------------------------------------------------------
 * goc_io_fs_unlink
 * ---------------------------------------------------------------------- */

/**
 * Deletes a file asynchronously.
 *
 * @param path Path to the file to delete.
 * @return A channel delivering the result of the operation.
 *         On error, the channel delivers a scalar error code.
 */
