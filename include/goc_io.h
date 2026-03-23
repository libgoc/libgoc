/*
 * include/goc_io.h — Async I/O wrappers for libgoc
 *
 * Provides channel-returning wrappers for libuv I/O functions across four
 * categories: Stream I/O, UDP datagrams, File System, and DNS resolution.
 *
 * Design
 * ------
 * Each libuv operation is exposed as a single function that returns goc_chan*.
 * The channel delivers the result when the I/O completes.  For operations
 * that return a composite result (goc_io_read_t*, goc_io_fs_stat_t*, etc.)
 * the channel delivers a GC-managed pointer.  For operations whose only
 * result is a scalar (status code, byte count, file descriptor), the channel
 * delivers the scalar encoded as (void*)(intptr_t)value.  The channel is
 * closed immediately after delivery.
 * Safe to call from both fiber context and OS thread context, and is
 * compatible with goc_alts() for select-style multiplexing.
 *
 * For streaming operations (goc_io_read_start, goc_io_udp_recv_start) the
 * channel delivers multiple values, one per callback.  Call the matching
 * stop function to halt I/O.
 *
 * Thread safety
 * -------------
 * File-system (uv_fs_*) and DNS (uv_getaddrinfo, uv_getnameinfo) operations
 * are safe to initiate from any thread; libuv routes them through its
 * internal worker-thread pool and fires the callback on the event loop.
 *
 * Stream and UDP operations (uv_write, uv_read_start, etc.) require the
 * libuv handle to be used from the event loop thread.  Stream/UDP wrappers
 * use a uv_async_t bridge so they can be called safely from fiber or
 * OS-thread context.
 *
 * Handle initialisation (uv_tcp_init, uv_pipe_init, uv_udp_init, etc.) is
 * the caller's responsibility.  Initialise handles directly on the event
 * loop thread using goc_scheduler() as the loop argument:
 *   uv_tcp_t* tcp = goc_malloc(sizeof(uv_tcp_t));
 *   uv_tcp_init(goc_scheduler(), tcp);
 *
 * Compile requirements: -std=c11
 *   Include this header explicitly: #include "goc_io.h"
 *   (goc.h does NOT automatically include goc_io.h)
 */

#ifndef GOC_IO_H
#define GOC_IO_H

#include <stddef.h>
#include <stdint.h>
#ifndef _WIN32
#  include <netdb.h>
#endif
#include <uv.h>
#include "goc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * I/O operation status
 * ====================================================================== */

typedef enum {
    GOC_IO_ERR =  0,  /* I/O operation failed                  */
    GOC_IO_OK  =  1,  /* I/O operation completed successfully   */
} goc_io_status_t;

/* =========================================================================
 * Result types
 * ====================================================================== */

/* -------------------------------------------------------------------------
 * Stream I/O
 * ---------------------------------------------------------------------- */

/**
 * goc_io_read_t — result of one read callback fired by goc_io_read_start.
 *
 * nread : bytes read (> 0) on a data event.  On error this is a negative
 *         libuv error code.
 * buf   : GC-managed buffer containing the data.  Valid when nread > 0;
 *         NULL when nread <= 0.  libgoc does not free buf.
 *
 * When nread < 0 the channel is already closed; this is the last value.
 */
typedef struct {
    ssize_t   nread;
    uv_buf_t* buf;
} goc_io_read_t;

/* -------------------------------------------------------------------------
 * UDP
 * ---------------------------------------------------------------------- */

/**
 * goc_io_udp_recv_t — result of one receive callback fired by
 * goc_io_udp_recv_start.
 *
 * nread : bytes received (> 0) on a datagram event.  On error this is a
 *         negative libuv error code.
 * buf   : GC-managed buffer containing the datagram data.  Valid when
 *         nread > 0; NULL otherwise.  libgoc does not free buf.
 * addr  : GC-managed copy of the source address.  NULL when nread <= 0 or
 *         no address was provided.  libgoc does not free addr.
 * flags : libuv-defined receive flags.
 *
 * When nread < 0 the channel is already closed; this is the last value.
 */
typedef struct {
    ssize_t          nread;
    uv_buf_t*        buf;
    struct sockaddr* addr;
    unsigned         flags;
} goc_io_udp_recv_t;

/* -------------------------------------------------------------------------
 * File system
 * ---------------------------------------------------------------------- */

/**
 * goc_io_fs_stat_t — result of goc_io_fs_stat.
 *
 * ok      : GOC_IO_OK on success, GOC_IO_ERR on failure.
 * statbuf : populated when ok == GOC_IO_OK.
 */
typedef struct {
    goc_io_status_t ok;
    uv_stat_t    statbuf;
} goc_io_fs_stat_t;

/* -------------------------------------------------------------------------
 * DNS & resolution
 * ---------------------------------------------------------------------- */

/**
 * goc_io_getaddrinfo_t — result of goc_io_getaddrinfo.
 *
 * ok  : GOC_IO_OK on success, GOC_IO_ERR on failure.
 * res : linked list of addrinfo results allocated by libuv.  The caller
 *       must release it with uv_freeaddrinfo(res) when done.
 *       NULL when ok != GOC_IO_OK.
 */
typedef struct {
    goc_io_status_t  ok;
    struct addrinfo* res;
} goc_io_getaddrinfo_t;

/**
 * goc_io_getnameinfo_t — result of goc_io_getnameinfo.
 *
 * ok       : GOC_IO_OK on success, GOC_IO_ERR on failure.
 * hostname : resolved host name (NUL-terminated).  Empty when ok != GOC_IO_OK.
 * service  : resolved service name (NUL-terminated).  Empty when ok != GOC_IO_OK.
 */
typedef struct {
    goc_io_status_t ok;
    char         hostname[NI_MAXHOST];
    char         service[NI_MAXSERV];
} goc_io_getnameinfo_t;

/* =========================================================================
 * 1. Stream I/O (TCP, Pipes, TTY)
 * ====================================================================== */

/**
 * goc_io_read_start() — Begin receiving data from a stream.
 *
 * stream : an initialised and connected libuv stream handle.
 *
 * Returns a channel that delivers goc_io_read_t* values, one per read
 * callback fired by libuv.  The channel is closed when EOF or an
 * unrecoverable error is encountered; the last delivered value carries the
 * error code in nread.
 * Call goc_io_read_stop() to stop reading before EOF and close the channel.
 *
 * Constraint: this function stores an internal context pointer in
 * stream->data for the lifetime of the read.  The caller must not use
 * stream->data for any other purpose while a read is in progress (i.e.
 * between goc_io_read_start() and the corresponding goc_io_read_stop()
 * call or channel close).
 *
 * Safe to call from any context (fiber or OS thread).
 */
goc_chan* goc_io_read_start(uv_stream_t* stream);

/**
 * goc_io_read_stop() — Stop reading from a stream previously started with
 * goc_io_read_start().
 *
 * Dispatches uv_read_stop() to the event loop thread and closes the
 * associated read channel.  Returns 0; the actual stop happens
 * asynchronously on the event loop thread.
 *
 * stream : the same handle passed to goc_io_read_start().
 *
 * Safe to call from any context.
 */
int goc_io_read_stop(uv_stream_t* stream);

/**
 * goc_io_write() — Initiate an async stream write; return result channel.
 *
 * handle : target stream handle.
 * bufs   : array of nbufs buffers to write.
 * nbufs  : number of buffers.
 *
 * Returns a channel that delivers (void*)(intptr_t)status when the write
 * completes; 0 on success, a negative libuv error code on failure.
 * The channel is closed after delivery.
 *
 * Safe to call from any context.
 */
goc_chan* goc_io_write(uv_stream_t* handle,
                       const uv_buf_t bufs[], unsigned int nbufs);

/**
 * goc_io_write2() — Initiate an async write with handle passing (IPC).
 *
 * Same as goc_io_write() with an additional send_handle for IPC streams.
 * Returns a channel delivering (void*)(intptr_t)status on completion.
 */
goc_chan* goc_io_write2(uv_stream_t* handle,
                        const uv_buf_t bufs[], unsigned int nbufs,
                        uv_stream_t* send_handle);

/**
 * goc_io_shutdown_stream() — Initiate a stream shutdown; return result channel.
 *
 * handle : stream to shut down (half-close the write side).
 *
 * Returns a channel delivering (void*)(intptr_t)status on completion;
 * 0 on success, a negative libuv error code on failure.
 */
goc_chan* goc_io_shutdown_stream(uv_stream_t* handle);

/**
 * goc_io_tcp_connect() — Initiate a TCP connection; return result channel.
 *
 * handle : an initialised and bound (optional) uv_tcp_t handle.
 * addr   : target address.
 *
 * Returns a channel delivering (void*)(intptr_t)status on completion;
 * 0 on success, a negative libuv error code on failure.
 */
goc_chan* goc_io_tcp_connect(uv_tcp_t* handle, const struct sockaddr* addr);

/**
 * goc_io_pipe_connect() — Initiate a pipe connect; return result channel.
 *
 * handle : an initialised uv_pipe_t handle.
 * name   : path to the named pipe / Unix domain socket.
 *
 * Returns a channel delivering (void*)(intptr_t)status on completion;
 * 0 on success, a negative libuv error code on failure.
 */
goc_chan* goc_io_pipe_connect(uv_pipe_t* handle, const char* name);

/* =========================================================================
 * 2. UDP (Datagrams)
 * ====================================================================== */

/**
 * goc_io_udp_send() — Initiate an async UDP send; return result channel.
 *
 * handle : an initialised uv_udp_t handle.
 * bufs   : array of nbufs buffers to send.
 * nbufs  : number of buffers.
 * addr   : destination address.
 *
 * Returns a channel delivering (void*)(intptr_t)status when the datagram
 * has been sent (or failed); 0 on success, negative libuv error on failure.
 */
goc_chan* goc_io_udp_send(uv_udp_t* handle,
                          const uv_buf_t bufs[], unsigned int nbufs,
                          const struct sockaddr* addr);

/**
 * goc_io_udp_recv_start() — Begin receiving UDP datagrams.
 *
 * handle : an initialised and bound uv_udp_t handle.
 *
 * Returns a channel that delivers goc_io_udp_recv_t* values, one per
 * datagram received.  The channel is closed on unrecoverable error; the
 * last delivered value carries the error code in nread.
 * Call goc_io_udp_recv_stop() to stop receiving and close the channel.
 */
goc_chan* goc_io_udp_recv_start(uv_udp_t* handle);

/**
 * goc_io_udp_recv_stop() — Stop receiving UDP datagrams.
 *
 * Dispatches uv_udp_recv_stop() to the event loop thread and closes the
 * associated receive channel.  Returns 0; the stop takes effect
 * asynchronously.
 */
int goc_io_udp_recv_stop(uv_udp_t* handle);

/* =========================================================================
 * 3. File System Operations
 * ====================================================================== */

/**
 * goc_io_fs_open() — Initiate an async file open; return result channel.
 *
 * path  : file path.
 * flags : open flags (UV_FS_O_RDONLY, UV_FS_O_WRONLY | UV_FS_O_CREAT, ...).
 * mode  : permission bits used when creating a new file.
 *
 * Returns a channel delivering (void*)(intptr_t)fd; fd >= 0 on success,
 * a negative libuv error code on failure.
 * Safe to call from any context.
 */
goc_chan* goc_io_fs_open(const char* path, int flags, int mode);

/**
 * goc_io_fs_close() — Initiate an async file close; return result channel.
 *
 * file : file descriptor returned by goc_io_fs_open.
 *
 * Returns a channel delivering (void*)(intptr_t)result; 0 on success,
 * a negative libuv error code on failure.
 */
goc_chan* goc_io_fs_close(uv_file file);

/**
 * goc_io_fs_read() — Initiate an async file read; return result channel.
 *
 * file   : open file descriptor.
 * bufs   : array of nbufs buffers to read into.
 * nbufs  : number of buffers.
 * offset : file offset (-1 to use current position).
 *
 * Returns a channel delivering (void*)(intptr_t)result; bytes read (>= 0)
 * on success, a negative libuv error code on failure.
 */
goc_chan* goc_io_fs_read(uv_file file,
                         const uv_buf_t bufs[], unsigned int nbufs,
                         int64_t offset);

/**
 * goc_io_fs_write() — Initiate an async file write; return result channel.
 *
 * file   : open file descriptor.
 * bufs   : array of nbufs buffers to write.
 * nbufs  : number of buffers.
 * offset : file offset (-1 to use current position).
 *
 * Returns a channel delivering (void*)(intptr_t)result; bytes written (>= 0)
 * on success, a negative libuv error code on failure.
 */
goc_chan* goc_io_fs_write(uv_file file,
                          const uv_buf_t bufs[], unsigned int nbufs,
                          int64_t offset);

/**
 * goc_io_fs_unlink() — Initiate an async file deletion; return result channel.
 *
 * path : path of the file to delete.
 *
 * Returns a channel delivering (void*)(intptr_t)result; 0 on success,
 * a negative libuv error code on failure.
 */
goc_chan* goc_io_fs_unlink(const char* path);

/**
 * goc_io_fs_stat() — Initiate an async file stat; return result channel.
 *
 * path : path of the file to stat.
 *
 * Returns a channel delivering goc_io_fs_stat_t*.
 */
goc_chan* goc_io_fs_stat(const char* path);

/**
 * goc_io_fs_rename() — Initiate an async file rename; return result channel.
 *
 * path     : current file path.
 * new_path : new file path.
 *
 * Returns a channel delivering (void*)(intptr_t)result; 0 on success,
 * a negative libuv error code on failure.
 */
goc_chan* goc_io_fs_rename(const char* path, const char* new_path);

/**
 * goc_io_fs_sendfile() — Initiate an async sendfile; return result channel.
 *
 * out_fd    : destination file descriptor.
 * in_fd     : source file descriptor.
 * in_offset : offset within the source file to start reading from.
 * length    : number of bytes to transfer.
 *
 * Returns a channel delivering (void*)(intptr_t)result; bytes transferred
 * (>= 0) on success, a negative libuv error code on failure.
 */
goc_chan* goc_io_fs_sendfile(uv_file out_fd, uv_file in_fd,
                             int64_t in_offset, size_t length);

/* =========================================================================
 * 4. DNS & Resolution
 * ====================================================================== */

/**
 * goc_io_getaddrinfo() — Initiate an async getaddrinfo; return result channel.
 *
 * node    : host name or numeric address string (may be NULL).
 * service : service name or port number string (may be NULL).
 * hints   : address hints (may be NULL).
 *
 * Returns a channel delivering goc_io_getaddrinfo_t*.  On success
 * (ok == GOC_IO_OK) the caller must release result->res with uv_freeaddrinfo().
 *
 * Safe to call from any context.
 */
goc_chan* goc_io_getaddrinfo(const char* node, const char* service,
                             const struct addrinfo* hints);

/**
 * goc_io_getnameinfo() — Initiate an async getnameinfo; return result channel.
 *
 * addr  : address to resolve.
 * flags : NI_* flags.
 *
 * Returns a channel delivering goc_io_getnameinfo_t*.
 *
 * Safe to call from any context.
 */
goc_chan* goc_io_getnameinfo(const struct sockaddr* addr, int flags);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GOC_IO_H */
