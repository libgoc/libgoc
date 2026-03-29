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
 * Handle initialisation (uv_tcp_init, uv_pipe_init, uv_udp_init, etc.) must
 * reach the event loop thread; use goc_io_tcp_init / goc_io_pipe_init /
 * goc_io_udp_init which dispatch there and register the handle automatically:
 *   uv_tcp_t* tcp = goc_malloc(sizeof(uv_tcp_t));
 *   int rc = goc_unbox_int(goc_take(goc_io_tcp_init(tcp))->val);
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
 * goc_io_fs_stat_t — result of goc_io_fs_stat / lstat / fstat.
 *
 * ok      : GOC_IO_OK on success, GOC_IO_ERR on failure.
 * statbuf : populated only when ok == GOC_IO_OK.
 */
typedef struct {
    goc_io_status_t ok;
    uv_stat_t    statbuf;
} goc_io_fs_stat_t;

/* Forward declaration — definition is opaque (lives in goc_io.c). */
typedef struct goc_io_fs_write_stream goc_io_fs_write_stream_t;

#include "goc_array.h"

/**
 * goc_io_fs_read_t — result of goc_io_fs_read.
 * nread < 0 signals a libuv error.
 * buf is a GC-managed char* containing the bytes read; valid when nread > 0; otherwise, buf is NULL.
 */
typedef struct {
    ssize_t  nread;
    char*    buf;
} goc_io_fs_read_t;

/**
 * goc_io_fs_event_t — result delivered by goc_io_fs_event_start on each change.
 */
typedef struct {
    goc_io_status_t  ok;
    const char*      filename;
    int              events;
} goc_io_fs_event_t;

/**
 * goc_io_fs_poll_t — result delivered by goc_io_fs_poll_start on each change.
 */
typedef struct {
    goc_io_status_t  ok;
    uv_stat_t        prev;
    uv_stat_t        curr;
} goc_io_fs_poll_t;

/**
 * goc_io_fs_dirent_t — one directory entry delivered inside goc_io_fs_readdir_t.
 */
typedef struct {
    const char*      name;
    uv_dirent_type_t type;
} goc_io_fs_dirent_t;

/**
 * goc_io_fs_readdir_t — result of goc_io_fs_readdir.
 * entries is a goc_array of goc_io_fs_dirent_t*.
 */
typedef struct {
    goc_io_status_t  ok;
    goc_array*       entries;
} goc_io_fs_readdir_t;

/**
 * goc_io_fs_path_t — result of goc_io_fs_mkdtemp, readlink, realpath.
 */
typedef struct {
    goc_io_status_t  ok;
    const char*      path;
} goc_io_fs_path_t;

/**
 * goc_io_fs_mkstemp_t — result of goc_io_fs_mkstemp.
 */
typedef struct {
    goc_io_status_t  ok;
    uv_file          fd;
    const char*      path;
} goc_io_fs_mkstemp_t;

/**
 * goc_io_fs_statfs_t — result of goc_io_fs_statfs.
 */
typedef struct {
    goc_io_status_t  ok;
    uv_statfs_t      statbuf;
} goc_io_fs_statfs_t;

/**
 * goc_io_fs_read_file_t — result of goc_io_fs_read_file.
 * data is a GC-managed null-terminated string containing the file contents.
 * NULL when ok != GOC_IO_OK.
 */
typedef struct {
    goc_io_status_t  ok;
    char*            data;
} goc_io_fs_read_file_t;

/**
 * goc_io_fs_read_chunk_t — one chunk delivered by goc_io_fs_read_stream_make.
 * status < 0 on error; data NULL on error/EOF.
 * data is a GC-managed char* of len bytes (not null-terminated).
 */
typedef struct {
    int     status;
    char*   data;
    size_t  len;
} goc_io_fs_read_chunk_t;

/**
 * goc_io_fs_write_stream_open_t — delivered by goc_io_fs_write_stream_make.
 * ws is NULL on failure.
 */
typedef struct {
    goc_io_status_t           ok;
    goc_io_fs_write_stream_t* ws;
} goc_io_fs_write_stream_open_t;

/**
 * goc_io_process_exit_t — delivered by the exit channel from goc_io_process_spawn.
 */
typedef struct {
    int64_t  exit_status;
    int      term_signal;
} goc_io_process_exit_t;

/**
 * goc_io_tty_winsize_t — result of goc_io_tty_get_winsize.
 */
typedef struct {
    goc_io_status_t  ok;
    int              width;
    int              height;
} goc_io_tty_winsize_t;

/**
 * goc_io_random_t — result of goc_io_random.
 * data is a goc_array of n bytes (each element is goc_box_int(byte)).
 */
typedef struct {
    goc_io_status_t  ok;
    goc_array*       data;
} goc_io_random_t;

/* -------------------------------------------------------------------------
 * DNS & resolution
 * ---------------------------------------------------------------------- */

/**
 * goc_io_getaddrinfo_t — result of goc_io_getaddrinfo.
 *
 * ok  : GOC_IO_OK on success, GOC_IO_ERR on failure.
 * res : linked list of addrinfo results allocated by libuv. The caller must release res with uv_freeaddrinfo(res) when done. NULL when ok != GOC_IO_OK.
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
 * a negative libuv error code on failure. On error, the channel delivers a scalar error code.
 */
goc_chan* goc_io_fs_close(uv_file file);

/**
 * goc_io_fs_read() — Initiate an async file read; return result channel.
 *
 * file   : open file descriptor.
 * len    : maximum number of bytes to read.
 * offset : file offset (-1 to use current position).
 *
 * Returns a channel delivering goc_io_fs_read_t*; nread < 0 on error.
 * On success, res->buf is a GC-managed char* of res->nread bytes.
 */
goc_chan* goc_io_fs_read(uv_file file, size_t len, int64_t offset);

/**
 * goc_io_fs_write() — Initiate an async file write; return result channel.
 *
 * file   : open file descriptor.
 * data   : pointer to string data.
 * len    : length of the string data.
 * offset : file offset (-1 to use current position).
 *
 * Returns a channel delivering goc_box_int(nwritten); negative on error.
 */
goc_chan* goc_io_fs_write(uv_file file, const char* data, size_t len, int64_t offset);

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
 * a negative libuv error code on failure. On error, the channel delivers a scalar error code.
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

/* =========================================================================
 * 5. GC handle lifetime management
 *
 * These functions allow libuv handles to be allocated on the GC heap
 * (via goc_malloc) instead of plain malloc.  Because libuv holds an opaque
 * internal reference to a handle until uv_close completes — a reference
 * invisible to Boehm GC — a GC-allocated handle would otherwise risk
 * premature collection.  goc_io_handle_register pins the handle in a
 * GC-visible root array; goc_io_handle_unregister / goc_io_handle_close remove it.
 *
 * Typical usage (preferred — safe from any thread):
 *
 *   uv_tcp_t* tcp = goc_malloc(sizeof(uv_tcp_t));
 *   int rc = goc_unbox_int(goc_take(goc_io_tcp_init(tcp))->val);
 *   // tcp is now initialised and registered; rc == 0 on success.
 *
 *   // ... later, to tear down:
 *   goc_io_handle_close((uv_handle_t*)tcp, NULL);  // unregisters automatically
 *
 * If you are already on the event loop thread and do not want a channel
 * round-trip, call uv_tcp_init() directly and then goc_io_handle_register().
 *
 * If you need a close callback AND are managing teardown yourself, you may
 * call uv_close() directly and then goc_io_handle_unregister() from within your
 * callback instead of using goc_io_handle_close().
 *
 * Note: goc_io_handle_close() overwrites handle->data from the loop thread
 * before calling uv_close.  Do not use handle->data after calling
 * goc_io_handle_close().
 * ====================================================================== */

/**
 * goc_io_tcp_init() — Initialise a GC-allocated uv_tcp_t from any thread.
 *
 * handle : GC-allocated uv_tcp_t (via goc_malloc).
 *
 * Dispatches uv_tcp_init() to the event loop thread, then registers the handle
 * with goc_io_handle_register().  Returns a channel delivering
 * (void*)(intptr_t)status; 0 on success, a negative libuv error on failure.
 * On success the handle is initialised and pinned as a GC root; call
 * goc_io_handle_close() to tear it down.
 *
 * Safe to call from any context.
 */
goc_chan* goc_io_tcp_init(uv_tcp_t* handle);

/**
 * goc_io_pipe_init() — Initialise a GC-allocated uv_pipe_t from any thread.
 *
 * handle : GC-allocated uv_pipe_t (via goc_malloc).
 * ipc    : non-zero if this pipe will be used for handle passing.
 *
 * Dispatches uv_pipe_init() to the event loop thread, then registers the
 * handle.  Returns a channel delivering (void*)(intptr_t)status; 0 on success,
 * a negative libuv error on failure.
 *
 * Safe to call from any context.
 */
goc_chan* goc_io_pipe_init(uv_pipe_t* handle, int ipc);

/**
 * goc_io_udp_init() — Initialise a GC-allocated uv_udp_t from any thread.
 *
 * handle : GC-allocated uv_udp_t (via goc_malloc).
 *
 * Dispatches uv_udp_init() to the event loop thread, then registers the
 * handle.  Returns a channel delivering (void*)(intptr_t)status; 0 on success,
 * a negative libuv error on failure.
 *
 * Safe to call from any context.
 */
goc_chan* goc_io_udp_init(uv_udp_t* handle);

/**
 * goc_io_tty_init() — Initialise a GC-allocated uv_tty_t from any thread.
 *
 * handle : GC-allocated uv_tty_t (via goc_malloc).
 * fd     : file descriptor for the TTY (e.g. 0=stdin, 1=stdout, 2=stderr).
 *
 * Dispatches uv_tty_init() to the event loop thread, then registers the
 * handle.  Returns a channel delivering (void*)(intptr_t)status; 0 on success,
 * a negative libuv error on failure.
 *
 * Safe to call from any context.
 */
goc_chan* goc_io_tty_init(uv_tty_t* handle, uv_file fd);

/**
 * goc_io_signal_init() — Initialise a GC-allocated uv_signal_t from any thread.
 *
 * handle : GC-allocated uv_signal_t (via goc_malloc).
 *
 * Dispatches uv_signal_init() to the event loop thread, then registers the
 * handle.  Returns a channel delivering (void*)(intptr_t)status; 0 on success,
 * a negative libuv error on failure.
 *
 * Safe to call from any context.
 */
goc_chan* goc_io_signal_init(uv_signal_t* handle);

/**
 * goc_io_fs_event_init() — Initialise a GC-allocated uv_fs_event_t from any thread.
 *
 * handle : GC-allocated uv_fs_event_t (via goc_malloc).
 *
 * Dispatches uv_fs_event_init() to the event loop thread, then registers the
 * handle.  Returns a channel delivering (void*)(intptr_t)status; 0 on success,
 * a negative libuv error on failure.
 *
 * Safe to call from any context.
 */
goc_chan* goc_io_fs_event_init(uv_fs_event_t* handle);

/**
 * goc_io_fs_poll_init() — Initialise a GC-allocated uv_fs_poll_t from any thread.
 *
 * handle : GC-allocated uv_fs_poll_t (via goc_malloc).
 *
 * Dispatches uv_fs_poll_init() to the event loop thread, then registers the
 * handle.  Returns a channel delivering (void*)(intptr_t)status; 0 on success,
 * a negative libuv error on failure.
 *
 * Safe to call from any context.
 */
goc_chan* goc_io_fs_poll_init(uv_fs_poll_t* handle);

/**
 * goc_io_process_spawn() — Spawn a child process from any thread.
 *
 * handle  : GC-allocated uv_process_t (via goc_malloc).
 * opts    : spawn options; pointer must remain valid until the channel
 *           delivers its value (i.e. until the caller takes from the
 *           returned channel).
 * exit_ch : if non-NULL, *exit_ch is set to a channel that delivers
 *           goc_io_process_exit_t* when the child exits.  Pass NULL to
 *           opt out of exit tracking (backward-compatible).
 *
 * Dispatches uv_spawn() to the event loop thread.  On success (rc == 0) the
 * handle is registered as a GC root and the child process is running.
 * Returns a channel delivering (void*)(intptr_t)status; 0 on success, a
 * negative libuv error on failure.
 *
 * Safe to call from any context.
 */
goc_chan* goc_io_process_spawn(uv_process_t* handle,
                               const uv_process_options_t* opts,
                               goc_chan** exit_ch);

/* =========================================================================
 * TCP server / socket options
 * ====================================================================== */

/** Dispatch uv_tcp_bind; delivers goc_box_int(status). */
goc_chan* goc_io_tcp_bind(uv_tcp_t* handle, const struct sockaddr* addr);

/**
 * goc_io_tcp_server_make() — Start listening on a bound TCP handle.
 *
 * Calls uv_listen; channel delivers a new goc_malloc-allocated + registered
 * uv_tcp_t* for each incoming connection.  Channel stays open until the
 * server handle is closed.
 */
goc_chan* goc_io_tcp_server_make(uv_tcp_t* handle, int backlog);

/** Dispatch uv_tcp_keepalive; delivers goc_box_int(status). */
goc_chan* goc_io_tcp_keepalive(uv_tcp_t* handle, int enable, unsigned int delay);

/** Dispatch uv_tcp_nodelay; delivers goc_box_int(status). */
goc_chan* goc_io_tcp_nodelay(uv_tcp_t* handle, int enable);

/** Dispatch uv_tcp_simultaneous_accepts; delivers goc_box_int(status). */
goc_chan* goc_io_tcp_simultaneous_accepts(uv_tcp_t* handle, int enable);

/* =========================================================================
 * Pipe server / bind
 * ====================================================================== */

/** Dispatch uv_pipe_bind; delivers goc_box_int(status). */
goc_chan* goc_io_pipe_bind(uv_pipe_t* handle, const char* name);

/**
 * goc_io_pipe_server_make() — Start listening on a bound pipe handle.
 *
 * Mirror of goc_io_tcp_server_make; delivers accepted uv_pipe_t* per
 * connection.  Channel stays open until the server handle is closed.
 */
goc_chan* goc_io_pipe_server_make(uv_pipe_t* handle, int backlog);

/* =========================================================================
 * UDP socket options
 * ====================================================================== */

/** Dispatch uv_udp_bind; delivers goc_box_int(status). */
goc_chan* goc_io_udp_bind(uv_udp_t* handle, const struct sockaddr* addr,
                          unsigned flags);

/** Dispatch uv_udp_connect; delivers goc_box_int(status). */
goc_chan* goc_io_udp_connect(uv_udp_t* handle, const struct sockaddr* addr);

/** Dispatch uv_udp_set_broadcast; delivers goc_box_int(status). */
goc_chan* goc_io_udp_set_broadcast(uv_udp_t* handle, int on);

/** Dispatch uv_udp_set_ttl; delivers goc_box_int(status). */
goc_chan* goc_io_udp_set_ttl(uv_udp_t* handle, int ttl);

/** Dispatch uv_udp_set_multicast_ttl; delivers goc_box_int(status). */
goc_chan* goc_io_udp_set_multicast_ttl(uv_udp_t* handle, int ttl);

/** Dispatch uv_udp_set_multicast_loop; delivers goc_box_int(status). */
goc_chan* goc_io_udp_set_multicast_loop(uv_udp_t* handle, int on);

/** Dispatch uv_udp_set_multicast_interface; delivers goc_box_int(status). */
goc_chan* goc_io_udp_set_multicast_interface(uv_udp_t* handle,
                                             const char* iface_addr);

/** Dispatch uv_udp_set_membership; delivers goc_box_int(status). */
goc_chan* goc_io_udp_set_membership(uv_udp_t* handle,
                                    const char* mcast_addr,
                                    const char* iface_addr,
                                    uv_membership membership);

/** Dispatch uv_udp_set_source_membership; delivers goc_box_int(status). */
goc_chan* goc_io_udp_set_source_membership(uv_udp_t* handle,
                                           const char* mcast_addr,
                                           const char* iface_addr,
                                           const char* source_addr,
                                           uv_membership membership);

/* =========================================================================
 * Extended File System Operations
 * ====================================================================== */

/** Dispatch uv_fs_lstat; delivers goc_io_fs_stat_t*. */
goc_chan* goc_io_fs_lstat(const char* path);

/** Dispatch uv_fs_fstat; delivers goc_io_fs_stat_t*. */
goc_chan* goc_io_fs_fstat(uv_file file);

/** Dispatch uv_fs_ftruncate; delivers goc_box_int(status). */
goc_chan* goc_io_fs_ftruncate(uv_file file, int64_t offset);

/** Open + ftruncate + close; delivers goc_box_int(status). */
goc_chan* goc_io_fs_truncate(const char* path, int64_t offset);

/** Dispatch uv_fs_copyfile; delivers goc_box_int(status). */
goc_chan* goc_io_fs_copyfile(const char* src, const char* dst, int flags);

/** Dispatch uv_fs_access; delivers goc_box_int(status). */
goc_chan* goc_io_fs_access(const char* path, int mode);

/** Dispatch uv_fs_chmod; delivers goc_box_int(status). */
goc_chan* goc_io_fs_chmod(const char* path, int mode);

/** Dispatch uv_fs_fchmod; delivers goc_box_int(status). */
goc_chan* goc_io_fs_fchmod(uv_file file, int mode);

/** Dispatch uv_fs_chown; delivers goc_box_int(status). */
goc_chan* goc_io_fs_chown(const char* path, uv_uid_t uid, uv_gid_t gid);

/** Dispatch uv_fs_fchown; delivers goc_box_int(status). */
goc_chan* goc_io_fs_fchown(uv_file file, uv_uid_t uid, uv_gid_t gid);

/** Dispatch uv_fs_utime; delivers goc_box_int(status). */
goc_chan* goc_io_fs_utime(const char* path, double atime, double mtime);

/** Dispatch uv_fs_futime; delivers goc_box_int(status). */
goc_chan* goc_io_fs_futime(uv_file file, double atime, double mtime);

/** Dispatch uv_fs_lutime; delivers goc_box_int(status). */
goc_chan* goc_io_fs_lutime(const char* path, double atime, double mtime);

/** Dispatch uv_fs_mkdir; delivers goc_box_int(status). */
goc_chan* goc_io_fs_mkdir(const char* path, int mode);

/** Dispatch uv_fs_rmdir; delivers goc_box_int(status). */
goc_chan* goc_io_fs_rmdir(const char* path);

/** Dispatch uv_fs_scandir + iterate; delivers goc_io_fs_readdir_t*. */
goc_chan* goc_io_fs_readdir(const char* path);

/** Dispatch uv_fs_mkdtemp; delivers goc_io_fs_path_t*. */
goc_chan* goc_io_fs_mkdtemp(const char* tpl);

/** Dispatch uv_fs_mkstemp; delivers goc_io_fs_mkstemp_t*. */
goc_chan* goc_io_fs_mkstemp(const char* tpl);

/** Dispatch uv_fs_link; delivers goc_box_int(status). */
goc_chan* goc_io_fs_link(const char* path, const char* new_path);

/** Dispatch uv_fs_symlink; delivers goc_box_int(status). */
goc_chan* goc_io_fs_symlink(const char* path, const char* new_path, int flags);

/** Dispatch uv_fs_readlink; delivers goc_io_fs_path_t*. */
goc_chan* goc_io_fs_readlink(const char* path);

/** Dispatch uv_fs_realpath; delivers goc_io_fs_path_t*. */
goc_chan* goc_io_fs_realpath(const char* path);

/** Dispatch uv_fs_fsync; delivers goc_box_int(status). */
goc_chan* goc_io_fs_fsync(uv_file file);

/** Dispatch uv_fs_fdatasync; delivers goc_box_int(status). */
goc_chan* goc_io_fs_fdatasync(uv_file file);

/** Dispatch uv_fs_statfs; delivers goc_io_fs_statfs_t*. */
goc_chan* goc_io_fs_statfs(const char* path);

/* =========================================================================
 * High-level file helpers (Node.js fs convenience API)
 * ====================================================================== */

/**
 * goc_io_fs_read_file() — Read entire file; delivers goc_io_fs_read_file_t*.
 * data is a goc_array of bytes (each element is goc_box_int(byte)).
 */
goc_chan* goc_io_fs_read_file(const char* path);

/**
 * goc_io_fs_write_file() — Open + write + close; delivers goc_box_int(status).
 * flags : open flags, e.g. UV_FS_O_WRONLY|UV_FS_O_CREAT|UV_FS_O_TRUNC.
 */
goc_chan* goc_io_fs_write_file(const char* path, const char* data, int flags);

/**
 * goc_io_fs_append_file() — Open (O_APPEND) + write + close;
 * delivers goc_box_int(status).
 */
goc_chan* goc_io_fs_append_file(const char* path, const char* data);

/**
 * goc_io_fs_read_stream_make() — Open file and stream chunks.
 *
 * Delivers successive goc_io_fs_read_chunk_t* values (one per chunk).
 * Channel closed after EOF or error.
 */
goc_chan* goc_io_fs_read_stream_make(const char* path, size_t chunk_size);

/**
 * goc_io_fs_write_stream_make() — Open file for streaming writes.
 *
 * Delivers goc_io_fs_write_stream_open_t*; ws is non-NULL on success.
 * Use goc_io_fs_write_stream_write / goc_io_fs_write_stream_end to write.
 */
goc_chan* goc_io_fs_write_stream_make(const char* path, int flags);

/**
 * goc_io_fs_write_stream_write() — Write a chunk to an open write stream.
 * Delivers goc_box_int(nwritten); negative on error.
 */
goc_chan* goc_io_fs_write_stream_write(goc_io_fs_write_stream_t* ws,
                                       const char* data, size_t len);

/**
 * goc_io_fs_write_stream_end() — Flush and close write stream.
 * Delivers goc_box_int(status).
 */
goc_chan* goc_io_fs_write_stream_end(goc_io_fs_write_stream_t* ws);

/* =========================================================================
 * Signal watch
 * ====================================================================== */

/**
 * goc_io_signal_start() — Begin watching for a signal.
 *
 * Channel delivers goc_box_int(signum) on each delivery.
 * Channel stays open until goc_io_signal_stop().
 *
 * Constraint: stores context in handle->data.
 */
goc_chan* goc_io_signal_start(uv_signal_t* handle, int signum);

/**
 * goc_io_signal_stop() — Stop watching for a signal and close the channel.
 * Returns 0; stop takes effect asynchronously.
 */
int goc_io_signal_stop(uv_signal_t* handle);

/* =========================================================================
 * FS event / FS poll watches
 * ====================================================================== */

/**
 * goc_io_fs_event_start() — Begin watching a path for filesystem changes.
 *
 * Calls uv_fs_event_start; channel delivers goc_io_fs_event_t* on each change.
 * Channel stays open until goc_io_fs_event_stop().
 *
 * Constraint: stores context in handle->data.
 */
goc_chan* goc_io_fs_event_start(uv_fs_event_t* handle, const char* path,
                                unsigned flags);

/**
 * goc_io_fs_event_stop() — Stop watching and close the channel.
 * Returns 0; stop takes effect asynchronously.
 */
int goc_io_fs_event_stop(uv_fs_event_t* handle);

/**
 * goc_io_fs_poll_start() — Begin polling a path for stat changes.
 *
 * Calls uv_fs_poll_start; channel delivers goc_io_fs_poll_t* on each change.
 * Channel stays open until goc_io_fs_poll_stop().
 *
 * Constraint: stores context in handle->data.
 */
goc_chan* goc_io_fs_poll_start(uv_fs_poll_t* handle, const char* path,
                               unsigned interval_ms);

/**
 * goc_io_fs_poll_stop() — Stop polling and close the channel.
 * Returns 0; stop takes effect asynchronously.
 */
int goc_io_fs_poll_stop(uv_fs_poll_t* handle);

/* =========================================================================
 * TTY mode / window size
 * ====================================================================== */

/** Dispatch uv_tty_set_mode to loop thread; delivers goc_box_int(status). */
goc_chan* goc_io_tty_set_mode(uv_tty_t* handle, uv_tty_mode_t mode);

/** Dispatch uv_tty_get_winsize to loop thread; delivers goc_io_tty_winsize_t*. */
goc_chan* goc_io_tty_get_winsize(uv_tty_t* handle);

/* =========================================================================
 * Process signals
 * ====================================================================== */

/** Dispatch uv_process_kill; delivers goc_box_int(status). */
goc_chan* goc_io_process_kill(uv_process_t* handle, int signum);

/** Dispatch uv_kill(pid, signum); delivers goc_box_int(status). */
goc_chan* goc_io_kill(int pid, int signum);

/* =========================================================================
 * Random bytes
 * ====================================================================== */

/**
 * goc_io_random() — Fill n bytes with cryptographically strong random data.
 *
 * Delivers goc_io_random_t*; data is a goc_array of n bytes.
 */
goc_chan* goc_io_random(size_t n, unsigned flags);

/**
 * goc_io_handle_register() — Pin a GC-allocated libuv handle as a GC root.
 *
 * All libuv handles used with libgoc must be GC-allocated (via goc_malloc)
 * and registered with this function immediately after uv_*_init().  The
 * handle is kept alive by the GC root array until goc_io_handle_unregister()
 * or goc_io_handle_close() removes it.
 *
 * Safe to call from any context.
 */
void goc_io_handle_register(uv_handle_t* handle);

/**
 * goc_io_handle_unregister() — Remove a GC-allocated handle from the root array.
 *
 * Call from inside your uv_close callback (before or after your own cleanup).
 * After this returns the GC may collect the handle once all other references
 * drop.
 *
 * Safe to call from any context.
 */
void goc_io_handle_unregister(uv_handle_t* handle);

/**
 * goc_io_handle_close() — Close a GC-allocated handle and unregister it.
 *
 * Dispatches uv_close(handle, ...) to the event loop thread and automatically
 * calls goc_io_handle_unregister() once the close completes, then forwards to
 * cb (if non-NULL).
 *
 * Prefer this over calling uv_close() + goc_io_handle_unregister() manually.
 *
 * Safe to call from any context (fiber or OS thread).
 *
 * Note: overwrites handle->data from the loop thread before calling uv_close.
 * Do not use handle->data after calling goc_io_handle_close().
 */
void goc_io_handle_close(uv_handle_t* handle, uv_close_cb cb);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GOC_IO_H */
