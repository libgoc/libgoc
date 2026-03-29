# libgoc Async I/O (`goc_io`)

> Async I/O wrappers for libgoc — channel-based libuv I/O across four categories: Stream I/O, UDP datagrams, File System, and DNS resolution.

**Header:** `#include "goc_io.h"`

`goc_io.h` is a separate header from `goc.h`; include both when using async I/O:

```c
#include "goc.h"
#include "goc_io.h"
```

---

## Table of Contents

- [Design](#design)
- [Thread Safety](#thread-safety)
- [Status Codes](#status-codes)
- [Memory helpers](#memory-helpers)
- [Result Types](#result-types)
- [1. Stream I/O (TCP, Pipes, TTY)](#1-stream-io-tcp-pipes-tty)
- [2. UDP (Datagrams)](#2-udp-datagrams)
- [3. File System Operations](#3-file-system-operations)
- [4. High-level File Helpers](#4-high-level-file-helpers)
- [5. DNS & Resolution](#5-dns--resolution)
- [6. Signals](#6-signals)
- [7. FS Events & FS Poll](#7-fs-events--fs-poll)
- [8. TTY](#8-tty)
- [9. Process](#9-process)
- [10. Misc](#10-misc)
- [11. GC handle lifetime management](#11-gc-handle-lifetime-management)

---

## Design

Each libuv operation is exposed as a single function that returns `goc_chan*`.
The channel delivers the result when the I/O completes. For streaming operations
(`goc_io_read_start`, `goc_io_udp_recv_start`) the channel delivers multiple
values, one per callback. Call the matching stop function to halt I/O.

The channel form is safe to call from both fiber context and OS thread context,
and is compatible with `goc_alts()` for select-style multiplexing across I/O
operations.

---

## Thread Safety

File-system (`uv_fs_*`) and DNS (`uv_getaddrinfo`, `uv_getnameinfo`) operations
are safe to initiate from any thread; libuv routes them through its internal
worker-thread pool and fires the callback on the event loop.

Stream and UDP operations (`uv_write`, `uv_read_start`, etc.) use a
`uv_async_t` bridge and are safe to call from fiber or OS-thread context.

Handle initialisation (`uv_tcp_init`, `uv_pipe_init`, `uv_udp_init`, etc.)
modifies internal loop state and must reach the event loop thread. Use the
provided init helpers (`goc_io_tcp_init`, `goc_io_pipe_init`, `goc_io_udp_init`,
`goc_io_tty_init`, `goc_io_signal_init`, `goc_io_fs_event_init`,
`goc_io_fs_poll_init`, `goc_io_process_spawn`) — they dispatch to the loop
thread and register the handle automatically:

```c
uv_tcp_t* tcp = goc_malloc(sizeof(uv_tcp_t));
int rc = goc_unbox_int(goc_take(goc_io_tcp_init(tcp))->val);
// ... later:
goc_io_handle_close((uv_handle_t*)tcp, NULL);  /* unregisters automatically */
```

See [GC handle lifetime management](#11-gc-handle-lifetime-management) for full details.

For **write / send** operations the caller must keep the `uv_buf_t` array and
all `buf.base` pointers valid until the result channel delivers its value (i.e.
the operation has completed). This matches the libuv contract.


---

## Status Codes

Composite result types that can succeed or fail carry a `goc_io_status_t ok` field:

```c
typedef enum {
    GOC_IO_ERR =  0,  /* I/O operation failed                 */
    GOC_IO_OK  =  1,  /* I/O operation completed successfully  */
} goc_io_status_t;
```

Scalar-returning operations (write, connect, open, etc.) use the raw
libuv convention: 0 or a non-negative value on success, a negative
`UV_E*` error code on failure.

---

## Memory helpers

`goc_io` examples use two allocators from `goc.h`:

| Function | Signature | Description |
|---|---|---|
| `goc_malloc` | `void* goc_malloc(size_t n)` | Allocate `n` bytes on the Boehm GC heap. Zero-initialised. Never returns NULL (aborts on failure). |
| `goc_realloc` | `void* goc_realloc(void* ptr, size_t n)` | Resize a GC-heap allocation to `n` bytes. `ptr` must have been returned by `goc_malloc` or `goc_realloc`, or may be NULL. Preserves existing content up to `min(old_size, n)` bytes. Never returns NULL (aborts on failure). |

Both return GC-managed pointers; callers need not free them.

---

## Result Types

### Scalar results

Single-result scalar operations encode their result directly in the channel value; decode with `goc_unbox_int(...)`:

| Operation | Channel delivers |
|---|---|
| `goc_io_write`, `goc_io_write2`, `goc_io_shutdown_stream`, `goc_io_tcp_connect`, `goc_io_pipe_connect`, `goc_io_udp_send`, `goc_io_fs_close`, `goc_io_fs_unlink`, `goc_io_fs_rename`, `goc_io_fs_ftruncate`, `goc_io_fs_truncate`, `goc_io_fs_copyfile`, `goc_io_fs_access`, `goc_io_fs_chmod`, `goc_io_fs_fchmod`, `goc_io_fs_chown`, `goc_io_fs_fchown`, `goc_io_fs_utime`, `goc_io_fs_futime`, `goc_io_fs_lutime`, `goc_io_fs_mkdir`, `goc_io_fs_rmdir`, `goc_io_fs_link`, `goc_io_fs_symlink`, `goc_io_fs_fsync`, `goc_io_fs_fdatasync`, `goc_io_fs_write_file`, `goc_io_fs_append_file`, `goc_io_fs_write_stream_write`, `goc_io_fs_write_stream_end`, `goc_io_tcp_bind`, `goc_io_tcp_keepalive`, `goc_io_tcp_nodelay`, `goc_io_tcp_simultaneous_accepts`, `goc_io_pipe_bind`, `goc_io_udp_bind`, `goc_io_udp_connect`, `goc_io_udp_set_broadcast`, `goc_io_udp_set_ttl`, `goc_io_udp_set_multicast_ttl`, `goc_io_udp_set_multicast_loop`, `goc_io_udp_set_multicast_interface`, `goc_io_udp_set_membership`, `goc_io_udp_set_source_membership`, `goc_io_tty_set_mode`, `goc_io_process_kill`, `goc_io_kill` | `goc_box_int(status)` — 0 on success, negative libuv error |
| `goc_io_fs_open` | `goc_box_int(fd)` — fd >= 0 on success, negative libuv error |
| `goc_io_fs_write`, `goc_io_fs_sendfile` | `goc_box_int(nwritten/result)` — bytes >= 0 on success, negative libuv error |
| `goc_io_signal_start` | `goc_box_int(signum)` per delivery |

### Composite results (channel delivers a GC-managed pointer)

```c
/* goc_io_read_t — result of one read callback fired by goc_io_read_start.
 * nread > 0: bytes read; nread < 0: libuv error (channel closed after this).
 * buf: GC-managed buffer. Valid when nread > 0; NULL otherwise.
 * libgoc does not free buf. */
typedef struct {
    ssize_t   nread;
    uv_buf_t* buf;
} goc_io_read_t;

/* goc_io_udp_recv_t — result of one receive callback fired by
 * goc_io_udp_recv_start.
 * nread > 0: bytes received; nread < 0: libuv error (channel closed).
 * buf: GC-managed buffer. addr: GC-managed source address.
 * libgoc does not free buf or addr. */
typedef struct {
    ssize_t          nread;
    uv_buf_t*        buf;
    struct sockaddr* addr;
    unsigned         flags;
} goc_io_udp_recv_t;

/* goc_io_fs_read_t — result of goc_io_fs_read.
 * nread < 0 signals a libuv error.
 * buf is a GC-managed char* of nread bytes; valid when nread > 0; NULL otherwise. */
typedef struct {
    ssize_t  nread;
    char*    buf;
} goc_io_fs_read_t;

/* goc_io_fs_stat_t — result of goc_io_fs_stat / goc_io_fs_lstat / goc_io_fs_fstat.
 * ok == GOC_IO_OK: success (statbuf populated).  ok == GOC_IO_ERR: failure. */
typedef struct {
    goc_io_status_t ok;
    uv_stat_t    statbuf;
} goc_io_fs_stat_t;

/* goc_io_fs_statfs_t — result of goc_io_fs_statfs. */
typedef struct {
    goc_io_status_t  ok;
    uv_statfs_t      statbuf;
} goc_io_fs_statfs_t;

/* goc_io_fs_readdir_t — result of goc_io_fs_readdir.
 * entries is a goc_array of goc_io_fs_dirent_t*. */
typedef struct {
    goc_io_status_t  ok;
    goc_array*       entries;
} goc_io_fs_readdir_t;

/* goc_io_fs_dirent_t — one directory entry in goc_io_fs_readdir_t. */
typedef struct {
    const char*      name;
    uv_dirent_type_t type;
} goc_io_fs_dirent_t;

/* goc_io_fs_path_t — result of goc_io_fs_mkdtemp, goc_io_fs_readlink,
 * goc_io_fs_realpath. */
typedef struct {
    goc_io_status_t  ok;
    const char*      path;
} goc_io_fs_path_t;

/* goc_io_fs_mkstemp_t — result of goc_io_fs_mkstemp. */
typedef struct {
    goc_io_status_t  ok;
    uv_file          fd;
    const char*      path;
} goc_io_fs_mkstemp_t;

/* goc_io_fs_read_file_t — result of goc_io_fs_read_file.
 * data is a GC-managed null-terminated string. NULL when ok != GOC_IO_OK. */
typedef struct {
    goc_io_status_t  ok;
    char*            data;
} goc_io_fs_read_file_t;

/* goc_io_fs_read_chunk_t — one chunk delivered by goc_io_fs_read_stream_make.
 * status < 0 on error; data NULL on error/EOF.
 * data is a GC-managed char* of len bytes (not null-terminated). */
typedef struct {
    int     status;
    char*   data;
    size_t  len;
} goc_io_fs_read_chunk_t;

/* goc_io_fs_write_stream_open_t — delivered by goc_io_fs_write_stream_make.
 * ws is NULL on failure. */
typedef struct {
    goc_io_status_t           ok;
    goc_io_fs_write_stream_t* ws;
} goc_io_fs_write_stream_open_t;

/* goc_io_fs_event_t — result delivered by goc_io_fs_event_start on each change. */
typedef struct {
    goc_io_status_t  ok;
    const char*      filename;
    int              events;
} goc_io_fs_event_t;

/* goc_io_fs_poll_t — result delivered by goc_io_fs_poll_start on each change. */
typedef struct {
    goc_io_status_t  ok;
    uv_stat_t        prev;
    uv_stat_t        curr;
} goc_io_fs_poll_t;

/* goc_io_tty_winsize_t — result of goc_io_tty_get_winsize. */
typedef struct {
    goc_io_status_t  ok;
    int              width;
    int              height;
} goc_io_tty_winsize_t;

/* goc_io_process_exit_t — delivered by the exit channel from goc_io_process_spawn. */
typedef struct {
    int64_t  exit_status;
    int      term_signal;
} goc_io_process_exit_t;

/* goc_io_random_t — result of goc_io_random.
 * data is a goc_array of n bytes (each element is goc_box_int(byte)). */
typedef struct {
    goc_io_status_t  ok;
    goc_array*       data;
} goc_io_random_t;

/* goc_io_getaddrinfo_t — result of goc_io_getaddrinfo.
 * ok == GOC_IO_OK: success (res populated).  ok == GOC_IO_ERR: failure.
 * Release res with uv_freeaddrinfo() when done. */
typedef struct {
    goc_io_status_t  ok;
    struct addrinfo* res;
} goc_io_getaddrinfo_t;

/* goc_io_getnameinfo_t — result of goc_io_getnameinfo.
 * ok == GOC_IO_OK: success (hostname/service populated). */
typedef struct {
    goc_io_status_t ok;
    char         hostname[NI_MAXHOST];
    char         service[NI_MAXSERV];
} goc_io_getnameinfo_t;
```

---

## 1. Stream I/O (TCP, Pipes, TTY)

### Read

| Function | Signature | Description |
|---|---|---|
| `goc_io_read_start` | `goc_chan* goc_io_read_start(uv_stream_t* stream)` | Begin receiving data from a stream. Returns a channel that delivers `goc_io_read_t*` values, one per read callback. The channel is closed on EOF or unrecoverable error; the last delivered value carries the error code in `nread`. Call `goc_io_read_stop()` to stop before EOF. Safe from any context. |
| `goc_io_read_stop` | `int goc_io_read_stop(uv_stream_t* stream)` | Dispatch `uv_read_stop()` to the event loop thread and close the read channel. Returns 0; the stop takes effect asynchronously. Safe from any context. |

```c
goc_chan* rch = goc_io_read_start(stream);
goc_val_t* v;
while ((v = goc_take(rch))->ok == GOC_OK) {
    goc_io_read_t* rd = (goc_io_read_t*)v->val;
    if (rd->nread < 0) break;          /* error */
    fwrite(rd->buf->base, 1, (size_t)rd->nread, stdout);
    /* buf is GC-managed; no free() needed */
}
```

### Write

| Function | Signature | Description |
|---|---|---|
| `goc_io_write` | `goc_chan* goc_io_write(uv_stream_t* handle, const uv_buf_t bufs[], unsigned int nbufs)` | Initiate an async stream write; return result channel delivering `goc_box_int(status)`. 0 on success, negative libuv error on failure. Safe from any context. |
| `goc_io_write2` | `goc_chan* goc_io_write2(uv_stream_t* handle, const uv_buf_t bufs[], unsigned int nbufs, uv_stream_t* send_handle)` | Initiate an async write with handle passing (IPC); return result channel delivering `goc_box_int(status)`. |

### Shutdown / Connect

| Function | Signature | Description |
|---|---|---|
| `goc_io_shutdown_stream` | `goc_chan* goc_io_shutdown_stream(uv_stream_t* handle)` | Initiate a stream half-close (write side); return result channel delivering `goc_box_int(status)`. 0 on success, negative libuv error on failure. |
| `goc_io_tcp_connect` | `goc_chan* goc_io_tcp_connect(uv_tcp_t* handle, const struct sockaddr* addr)` | Initiate a TCP connection; return result channel delivering `goc_box_int(status)`. 0 on success, negative libuv error on failure. |
| `goc_io_pipe_connect` | `goc_chan* goc_io_pipe_connect(uv_pipe_t* handle, const char* name)` | Initiate a pipe (named pipe / Unix socket) connect; return result channel delivering `goc_box_int(status)`. 0 on success, negative libuv error on failure. |

### TCP socket options / server

| Function | Signature | Description |
|---|---|---|
| `goc_io_tcp_bind` | `goc_chan* goc_io_tcp_bind(uv_tcp_t* handle, const struct sockaddr* addr)` | Dispatch `uv_tcp_bind`; delivers `goc_box_int(status)`. |
| `goc_io_tcp_server_make` | `goc_chan* goc_io_tcp_server_make(uv_tcp_t* handle, int backlog)` | Start listening on a bound TCP handle. Calls `uv_listen`; channel delivers a new `goc_malloc`-allocated and registered `uv_tcp_t*` for each incoming connection. Channel stays open until the server handle is closed. |
| `goc_io_tcp_keepalive` | `goc_chan* goc_io_tcp_keepalive(uv_tcp_t* handle, int enable, unsigned int delay)` | Dispatch `uv_tcp_keepalive`; delivers `goc_box_int(status)`. |
| `goc_io_tcp_nodelay` | `goc_chan* goc_io_tcp_nodelay(uv_tcp_t* handle, int enable)` | Dispatch `uv_tcp_nodelay`; delivers `goc_box_int(status)`. |
| `goc_io_tcp_simultaneous_accepts` | `goc_chan* goc_io_tcp_simultaneous_accepts(uv_tcp_t* handle, int enable)` | Dispatch `uv_tcp_simultaneous_accepts`; delivers `goc_box_int(status)`. |

### Pipe server / bind

| Function | Signature | Description |
|---|---|---|
| `goc_io_pipe_bind` | `goc_chan* goc_io_pipe_bind(uv_pipe_t* handle, const char* name)` | Dispatch `uv_pipe_bind`; delivers `goc_box_int(status)`. |
| `goc_io_pipe_server_make` | `goc_chan* goc_io_pipe_server_make(uv_pipe_t* handle, int backlog)` | Mirror of `goc_io_tcp_server_make`; delivers accepted `uv_pipe_t*` per connection. Channel stays open until the server handle is closed. |

---

## 2. UDP (Datagrams)

| Function | Signature | Description |
|---|---|---|
| `goc_io_udp_send` | `goc_chan* goc_io_udp_send(uv_udp_t* handle, const uv_buf_t bufs[], unsigned int nbufs, const struct sockaddr* addr)` | Initiate an async UDP send; return result channel delivering `goc_box_int(status)`. 0 on success, negative libuv error on failure. |
| `goc_io_udp_recv_start` | `goc_chan* goc_io_udp_recv_start(uv_udp_t* handle)` | Begin receiving UDP datagrams. Returns a channel delivering `goc_io_udp_recv_t*` values, one per datagram. Channel is closed on unrecoverable error. Call `goc_io_udp_recv_stop()` to stop. |
| `goc_io_udp_recv_stop` | `int goc_io_udp_recv_stop(uv_udp_t* handle)` | Stop receiving UDP datagrams and close the receive channel. Returns 0; takes effect asynchronously. |

### UDP socket options

| Function | Signature | Description |
|---|---|---|
| `goc_io_udp_bind` | `goc_chan* goc_io_udp_bind(uv_udp_t* handle, const struct sockaddr* addr, unsigned flags)` | Dispatch `uv_udp_bind`; delivers `goc_box_int(status)`. |
| `goc_io_udp_connect` | `goc_chan* goc_io_udp_connect(uv_udp_t* handle, const struct sockaddr* addr)` | Dispatch `uv_udp_connect`; delivers `goc_box_int(status)`. |
| `goc_io_udp_set_broadcast` | `goc_chan* goc_io_udp_set_broadcast(uv_udp_t* handle, int on)` | Dispatch `uv_udp_set_broadcast`; delivers `goc_box_int(status)`. |
| `goc_io_udp_set_ttl` | `goc_chan* goc_io_udp_set_ttl(uv_udp_t* handle, int ttl)` | Dispatch `uv_udp_set_ttl`; delivers `goc_box_int(status)`. |
| `goc_io_udp_set_multicast_ttl` | `goc_chan* goc_io_udp_set_multicast_ttl(uv_udp_t* handle, int ttl)` | Dispatch `uv_udp_set_multicast_ttl`; delivers `goc_box_int(status)`. |
| `goc_io_udp_set_multicast_loop` | `goc_chan* goc_io_udp_set_multicast_loop(uv_udp_t* handle, int on)` | Dispatch `uv_udp_set_multicast_loop`; delivers `goc_box_int(status)`. |
| `goc_io_udp_set_multicast_interface` | `goc_chan* goc_io_udp_set_multicast_interface(uv_udp_t* handle, const char* iface_addr)` | Dispatch `uv_udp_set_multicast_interface`; delivers `goc_box_int(status)`. |
| `goc_io_udp_set_membership` | `goc_chan* goc_io_udp_set_membership(uv_udp_t* handle, const char* mcast_addr, const char* iface_addr, uv_membership membership)` | Dispatch `uv_udp_set_membership`; delivers `goc_box_int(status)`. |
| `goc_io_udp_set_source_membership` | `goc_chan* goc_io_udp_set_source_membership(uv_udp_t* handle, const char* mcast_addr, const char* iface_addr, const char* source_addr, uv_membership membership)` | Dispatch `uv_udp_set_source_membership`; delivers `goc_box_int(status)`. |

---

## 3. File System Operations

All file-system functions are safe to call from any context (fiber or OS thread).

| Function | Signature | Description |
|---|---|---|
| `goc_io_fs_open` | `goc_chan* goc_io_fs_open(const char* path, int flags, int mode)` | Async file open; channel delivers `goc_box_int(fd)`. fd >= 0 on success, negative libuv error on failure. Use `UV_FS_O_*` flags (e.g. `UV_FS_O_RDONLY`, `UV_FS_O_WRONLY \| UV_FS_O_CREAT`). |
| `goc_io_fs_close` | `goc_chan* goc_io_fs_close(uv_file file)` | Async file close; channel delivers `goc_box_int(status)`. 0 on success, negative libuv error on failure. |
| `goc_io_fs_read` | `goc_chan* goc_io_fs_read(uv_file file, size_t len, int64_t offset)` | Async file read up to `len` bytes; channel delivers `goc_io_fs_read_t*`. `nread < 0` on error; on success `res->buf` is a GC-managed `char*` of `res->nread` bytes. Pass `offset == -1` to use the current file position. |
| `goc_io_fs_write` | `goc_chan* goc_io_fs_write(uv_file file, const char* data, size_t len, int64_t offset)` | Async file write; channel delivers `goc_box_int(nwritten)`. Bytes written >= 0 on success, negative libuv error on failure. |
| `goc_io_fs_unlink` | `goc_chan* goc_io_fs_unlink(const char* path)` | Async file deletion; channel delivers `goc_box_int(status)`. 0 on success, negative libuv error on failure. |
| `goc_io_fs_stat` | `goc_chan* goc_io_fs_stat(const char* path)` | Async file stat; channel delivers `goc_io_fs_stat_t*`. |
| `goc_io_fs_lstat` | `goc_chan* goc_io_fs_lstat(const char* path)` | Like `goc_io_fs_stat` but does not follow symlinks; delivers `goc_io_fs_stat_t*`. |
| `goc_io_fs_fstat` | `goc_chan* goc_io_fs_fstat(uv_file file)` | Stat an open file descriptor; delivers `goc_io_fs_stat_t*`. |
| `goc_io_fs_rename` | `goc_chan* goc_io_fs_rename(const char* path, const char* new_path)` | Async file rename; channel delivers `goc_box_int(status)`. 0 on success, negative libuv error on failure. |
| `goc_io_fs_sendfile` | `goc_chan* goc_io_fs_sendfile(uv_file out_fd, uv_file in_fd, int64_t in_offset, size_t length)` | Async zero-copy file transfer; channel delivers `goc_box_int(result)`. Bytes transferred >= 0 on success, negative libuv error on failure. |
| `goc_io_fs_ftruncate` | `goc_chan* goc_io_fs_ftruncate(uv_file file, int64_t offset)` | Truncate open file to `offset` bytes; delivers `goc_box_int(status)`. |
| `goc_io_fs_truncate` | `goc_chan* goc_io_fs_truncate(const char* path, int64_t offset)` | Open + ftruncate + close; delivers `goc_box_int(status)`. |
| `goc_io_fs_copyfile` | `goc_chan* goc_io_fs_copyfile(const char* src, const char* dst, int flags)` | Dispatch `uv_fs_copyfile`; delivers `goc_box_int(status)`. |
| `goc_io_fs_access` | `goc_chan* goc_io_fs_access(const char* path, int mode)` | Dispatch `uv_fs_access`; delivers `goc_box_int(status)`. |
| `goc_io_fs_chmod` | `goc_chan* goc_io_fs_chmod(const char* path, int mode)` | Dispatch `uv_fs_chmod`; delivers `goc_box_int(status)`. |
| `goc_io_fs_fchmod` | `goc_chan* goc_io_fs_fchmod(uv_file file, int mode)` | Dispatch `uv_fs_fchmod`; delivers `goc_box_int(status)`. |
| `goc_io_fs_chown` | `goc_chan* goc_io_fs_chown(const char* path, uv_uid_t uid, uv_gid_t gid)` | Dispatch `uv_fs_chown`; delivers `goc_box_int(status)`. |
| `goc_io_fs_fchown` | `goc_chan* goc_io_fs_fchown(uv_file file, uv_uid_t uid, uv_gid_t gid)` | Dispatch `uv_fs_fchown`; delivers `goc_box_int(status)`. |
| `goc_io_fs_utime` | `goc_chan* goc_io_fs_utime(const char* path, double atime, double mtime)` | Dispatch `uv_fs_utime`; delivers `goc_box_int(status)`. |
| `goc_io_fs_futime` | `goc_chan* goc_io_fs_futime(uv_file file, double atime, double mtime)` | Dispatch `uv_fs_futime`; delivers `goc_box_int(status)`. |
| `goc_io_fs_lutime` | `goc_chan* goc_io_fs_lutime(const char* path, double atime, double mtime)` | Dispatch `uv_fs_lutime`; delivers `goc_box_int(status)`. |
| `goc_io_fs_mkdir` | `goc_chan* goc_io_fs_mkdir(const char* path, int mode)` | Dispatch `uv_fs_mkdir`; delivers `goc_box_int(status)`. |
| `goc_io_fs_rmdir` | `goc_chan* goc_io_fs_rmdir(const char* path)` | Dispatch `uv_fs_rmdir`; delivers `goc_box_int(status)`. |
| `goc_io_fs_readdir` | `goc_chan* goc_io_fs_readdir(const char* path)` | Dispatch `uv_fs_scandir` + iterate; delivers `goc_io_fs_readdir_t*`. |
| `goc_io_fs_mkdtemp` | `goc_chan* goc_io_fs_mkdtemp(const char* tpl)` | Dispatch `uv_fs_mkdtemp`; delivers `goc_io_fs_path_t*`. |
| `goc_io_fs_mkstemp` | `goc_chan* goc_io_fs_mkstemp(const char* tpl)` | Dispatch `uv_fs_mkstemp`; delivers `goc_io_fs_mkstemp_t*`. |
| `goc_io_fs_link` | `goc_chan* goc_io_fs_link(const char* path, const char* new_path)` | Dispatch `uv_fs_link`; delivers `goc_box_int(status)`. |
| `goc_io_fs_symlink` | `goc_chan* goc_io_fs_symlink(const char* path, const char* new_path, int flags)` | Dispatch `uv_fs_symlink`; delivers `goc_box_int(status)`. |
| `goc_io_fs_readlink` | `goc_chan* goc_io_fs_readlink(const char* path)` | Dispatch `uv_fs_readlink`; delivers `goc_io_fs_path_t*`. |
| `goc_io_fs_realpath` | `goc_chan* goc_io_fs_realpath(const char* path)` | Dispatch `uv_fs_realpath`; delivers `goc_io_fs_path_t*`. |
| `goc_io_fs_fsync` | `goc_chan* goc_io_fs_fsync(uv_file file)` | Dispatch `uv_fs_fsync`; delivers `goc_box_int(status)`. |
| `goc_io_fs_fdatasync` | `goc_chan* goc_io_fs_fdatasync(uv_file file)` | Dispatch `uv_fs_fdatasync`; delivers `goc_box_int(status)`. |
| `goc_io_fs_statfs` | `goc_chan* goc_io_fs_statfs(const char* path)` | Dispatch `uv_fs_statfs`; delivers `goc_io_fs_statfs_t*`. |

**Example — open, write, read, close (fiber context)**

```c
#include "goc.h"
#include "goc_io.h"

static void file_fiber(void* _) {
    /* Open */
    goc_val_t* vfd = goc_take(goc_io_fs_open("/tmp/hello.txt",
                                              UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC,
                                              0644));
    uv_file fd = (uv_file)goc_unbox_int(vfd->val);
    if (fd < 0) return;

    /* Write */
    const char data[] = "hello, libgoc\n";
    goc_val_t* vw = goc_take(goc_io_fs_write(fd, data, sizeof(data) - 1, 0));
    (void)vw;

    /* Close */
    goc_take(goc_io_fs_close(fd));
}

int main(void) {
    goc_init();
    goc_go(file_fiber, NULL);
    goc_shutdown();
    return 0;
}
```

---

## 4. High-level File Helpers

Convenience wrappers that compose open/read/write/close into a single call. Safe from any context.

| Function | Signature | Description |
|---|---|---|
| `goc_io_fs_read_file` | `goc_chan* goc_io_fs_read_file(const char* path)` | Read entire file; delivers `goc_io_fs_read_file_t*`. `data` is a GC-managed null-terminated string on success; NULL on failure. |
| `goc_io_fs_write_file` | `goc_chan* goc_io_fs_write_file(const char* path, const char* data, int flags)` | Open + write + close; delivers `goc_box_int(status)`. Use `flags` to control open behaviour (e.g. `UV_FS_O_WRONLY\|UV_FS_O_CREAT\|UV_FS_O_TRUNC`). |
| `goc_io_fs_append_file` | `goc_chan* goc_io_fs_append_file(const char* path, const char* data)` | Open with `O_APPEND` + write + close; delivers `goc_box_int(status)`. |
| `goc_io_fs_read_stream_make` | `goc_chan* goc_io_fs_read_stream_make(const char* path, size_t chunk_size)` | Open file and stream chunks. Delivers successive `goc_io_fs_read_chunk_t*` values (one per chunk). Channel is closed after EOF or error. |
| `goc_io_fs_write_stream_make` | `goc_chan* goc_io_fs_write_stream_make(const char* path, int flags)` | Open file for streaming writes. Delivers `goc_io_fs_write_stream_open_t*`; `ws` is non-NULL on success. |
| `goc_io_fs_write_stream_write` | `goc_chan* goc_io_fs_write_stream_write(goc_io_fs_write_stream_t* ws, const char* data, size_t len)` | Write a chunk to an open write stream; delivers `goc_box_int(nwritten)`. Negative on error. |
| `goc_io_fs_write_stream_end` | `goc_chan* goc_io_fs_write_stream_end(goc_io_fs_write_stream_t* ws)` | Flush and close write stream; delivers `goc_box_int(status)`. |

---

## 5. DNS & Resolution

Both functions are safe to call from any context.

| Function | Signature | Description |
|---|---|---|
| `goc_io_getaddrinfo` | `goc_chan* goc_io_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints)` | Async `getaddrinfo`; channel delivers `goc_io_getaddrinfo_t*`. On success `result->res` is a libuv-allocated linked list; release with `uv_freeaddrinfo(result->res)`. |
| `goc_io_getnameinfo` | `goc_chan* goc_io_getnameinfo(const struct sockaddr* addr, int flags)` | Async `getnameinfo`; channel delivers `goc_io_getnameinfo_t*` with `hostname` and `service` strings. |

**Example — resolve a hostname (fiber context)**

```c
#include "goc.h"
#include "goc_io.h"
#include <stdio.h>
#include <netdb.h>

static void dns_fiber(void* _) {
    goc_val_t* v = goc_take(goc_io_getaddrinfo("example.com", "https", NULL));
    goc_io_getaddrinfo_t* result = (goc_io_getaddrinfo_t*)v->val;
    if (result->ok == GOC_IO_OK) {
        char host[256];
        getnameinfo(result->res->ai_addr, result->res->ai_addrlen,
                    host, sizeof(host), NULL, 0, NI_NUMERICHOST);
        printf("example.com → %s\n", host);
        uv_freeaddrinfo(result->res);
    } else {
        fprintf(stderr, "DNS error (getaddrinfo failed)\n");
    }
}

int main(void) {
    goc_init();
    goc_go(dns_fiber, NULL);
    goc_shutdown();
    return 0;
}
```

---

## 6. Signals

| Function | Signature | Description |
|---|---|---|
| `goc_io_signal_start` | `goc_chan* goc_io_signal_start(uv_signal_t* handle, int signum)` | Begin watching for `signum`. Channel delivers `goc_box_int(signum)` on each signal delivery. Channel stays open until `goc_io_signal_stop()`. Stores context in `handle->data`. |
| `goc_io_signal_stop` | `int goc_io_signal_stop(uv_signal_t* handle)` | Stop watching and close the channel. Returns 0; stop takes effect asynchronously. |

---

## 7. FS Events & FS Poll

| Function | Signature | Description |
|---|---|---|
| `goc_io_fs_event_start` | `goc_chan* goc_io_fs_event_start(uv_fs_event_t* handle, const char* path, unsigned flags)` | Begin watching `path` for filesystem changes. Calls `uv_fs_event_start`; channel delivers `goc_io_fs_event_t*` on each change. Channel stays open until `goc_io_fs_event_stop()`. Stores context in `handle->data`. |
| `goc_io_fs_event_stop` | `int goc_io_fs_event_stop(uv_fs_event_t* handle)` | Stop watching and close the channel. Returns 0; stop takes effect asynchronously. |
| `goc_io_fs_poll_start` | `goc_chan* goc_io_fs_poll_start(uv_fs_poll_t* handle, const char* path, unsigned interval_ms)` | Begin polling `path` for stat changes at `interval_ms` millisecond intervals. Calls `uv_fs_poll_start`; channel delivers `goc_io_fs_poll_t*` on each change. Channel stays open until `goc_io_fs_poll_stop()`. Stores context in `handle->data`. |
| `goc_io_fs_poll_stop` | `int goc_io_fs_poll_stop(uv_fs_poll_t* handle)` | Stop polling and close the channel. Returns 0; stop takes effect asynchronously. |

---

## 8. TTY

| Function | Signature | Description |
|---|---|---|
| `goc_io_tty_set_mode` | `goc_chan* goc_io_tty_set_mode(uv_tty_t* handle, uv_tty_mode_t mode)` | Dispatch `uv_tty_set_mode` to the loop thread; delivers `goc_box_int(status)`. |
| `goc_io_tty_get_winsize` | `goc_chan* goc_io_tty_get_winsize(uv_tty_t* handle)` | Dispatch `uv_tty_get_winsize` to the loop thread; delivers `goc_io_tty_winsize_t*`. |

---

## 9. Process

| Function | Signature | Description |
|---|---|---|
| `goc_io_process_spawn` | `goc_chan* goc_io_process_spawn(uv_process_t* handle, const uv_process_options_t* opts, goc_chan** exit_ch)` | Dispatch `uv_spawn` to the loop thread. `opts` must remain valid until the channel delivers. If `exit_ch` is non-NULL, `*exit_ch` is set to a channel that delivers `goc_io_process_exit_t*` when the child exits; pass NULL to opt out of exit tracking. Channel delivers `goc_box_int(status)`; 0 on success. Safe from any context. |
| `goc_io_process_kill` | `goc_chan* goc_io_process_kill(uv_process_t* handle, int signum)` | Dispatch `uv_process_kill`; delivers `goc_box_int(status)`. |
| `goc_io_kill` | `goc_chan* goc_io_kill(int pid, int signum)` | Dispatch `uv_kill(pid, signum)`; delivers `goc_box_int(status)`. |

---

## 10. Misc

| Function | Signature | Description |
|---|---|---|
| `goc_io_random` | `goc_chan* goc_io_random(size_t n, unsigned flags)` | Fill `n` bytes with cryptographically strong random data. Delivers `goc_io_random_t*`; `data` is a `goc_array` of `n` bytes (each element is `goc_box_int(byte)`). |

---

## 11. GC handle lifetime management

libuv holds an opaque internal reference to every handle until `uv_close`
completes.  That reference is invisible to Boehm GC, so a handle allocated
with `goc_malloc` would risk premature collection.  These functions pin the
handle in a GC-visible root array for the duration of its libuv lifetime.

### Handle init helpers

| Function | Signature | Description |
|---|---|---|
| `goc_io_tcp_init` | `goc_chan* goc_io_tcp_init(uv_tcp_t* handle)` | Dispatch `uv_tcp_init` to the loop thread and register the handle. Channel delivers `goc_box_int(status)`; 0 on success. Safe from any context. |
| `goc_io_pipe_init` | `goc_chan* goc_io_pipe_init(uv_pipe_t* handle, int ipc)` | Dispatch `uv_pipe_init` to the loop thread and register the handle. Channel delivers `goc_box_int(status)`; 0 on success. Safe from any context. |
| `goc_io_udp_init` | `goc_chan* goc_io_udp_init(uv_udp_t* handle)` | Dispatch `uv_udp_init` to the loop thread and register the handle. Channel delivers `goc_box_int(status)`; 0 on success. Safe from any context. |
| `goc_io_tty_init` | `goc_chan* goc_io_tty_init(uv_tty_t* handle, uv_file fd)` | Dispatch `uv_tty_init` to the loop thread and register the handle. `fd`: 0=stdin, 1=stdout, 2=stderr. Channel delivers `goc_box_int(status)`; 0 on success. Safe from any context. |
| `goc_io_signal_init` | `goc_chan* goc_io_signal_init(uv_signal_t* handle)` | Dispatch `uv_signal_init` to the loop thread and register the handle. Channel delivers `goc_box_int(status)`; 0 on success. Safe from any context. |
| `goc_io_fs_event_init` | `goc_chan* goc_io_fs_event_init(uv_fs_event_t* handle)` | Dispatch `uv_fs_event_init` to the loop thread and register the handle. Channel delivers `goc_box_int(status)`; 0 on success. Safe from any context. |
| `goc_io_fs_poll_init` | `goc_chan* goc_io_fs_poll_init(uv_fs_poll_t* handle)` | Dispatch `uv_fs_poll_init` to the loop thread and register the handle. Channel delivers `goc_box_int(status)`; 0 on success. Safe from any context. |
| `goc_io_process_spawn` | `goc_chan* goc_io_process_spawn(uv_process_t* handle, const uv_process_options_t* opts, goc_chan** exit_ch)` | Dispatch `uv_spawn` to the loop thread. On success the process is running and the handle is registered. The `opts` pointer must remain valid until the channel delivers. Channel delivers `goc_box_int(status)`; 0 on success. Safe from any context. |

### Lifetime management

| Function | Signature | Description |
|---|---|---|
| `goc_io_handle_register` | `void goc_io_handle_register(uv_handle_t* handle)` | Pin a GC-allocated handle as a GC root. Only needed if you call `uv_*_init` yourself (e.g. from the loop thread). Safe from any context. |
| `goc_io_handle_unregister` | `void goc_io_handle_unregister(uv_handle_t* handle)` | Remove the handle from the root array. Call from inside your `uv_close` callback. Safe from any context. |
| `goc_io_handle_close` | `void goc_io_handle_close(uv_handle_t* handle, uv_close_cb cb)` | Dispatch `uv_close` to the loop thread and automatically unregister on completion, then forward to `cb` (may be NULL). Safe from any context. Overwrites `handle->data` from the loop thread before calling `uv_close`. |

**Example — TCP handle from fiber context**

```c
#include "goc.h"
#include "goc_io.h"

static void tcp_fiber(void* _) {
    uv_tcp_t* tcp = goc_malloc(sizeof(uv_tcp_t));
    int rc = goc_unbox_int(goc_take(goc_io_tcp_init(tcp))->val);
    if (rc < 0) return;  /* init failed */

    /* ... connect, read, write ... */

    goc_io_handle_close((uv_handle_t*)tcp, NULL);
}
```

If you need the close callback for your own cleanup and are calling `uv_close`
directly from the loop thread, use `goc_io_handle_unregister` from within it
instead of `goc_io_handle_close`:

```c
static void on_closed(uv_handle_t* h) {
    goc_io_handle_unregister(h);
    /* h is GC-managed; no free() needed */
}
uv_close((uv_handle_t*)tcp, on_closed);  /* must be on loop thread */
```

---

**Select across multiple I/O operations (`goc_alts`)**

All `goc_io_*` functions return plain `goc_chan*` values and are first-class arms in
`goc_alts()`, enabling deadline-aware I/O:

```c
#include "goc.h"
#include "goc_io.h"

static void io_fiber(void* _) {
    goc_chan* open_ch    = goc_io_fs_open("/tmp/data.txt", UV_FS_O_RDONLY, 0);
    goc_chan* timeout_ch = goc_timeout(500);   /* from goc.h */

    goc_alt_op ops[] = {
        { .ch = open_ch,    .op_kind = GOC_ALT_TAKE },
        { .ch = timeout_ch, .op_kind = GOC_ALT_TAKE },
    };
    goc_alts_result* r = goc_alts(ops, 2);

    if (r->ch == timeout_ch) {
        printf("open timed out\n");
    } else {
        uv_file fd = (uv_file)goc_unbox_int(r->value.val);
        if (fd >= 0)
            printf("opened fd=%d\n", (int)fd);
    }
}

int main(void) {
    goc_init();
    goc_go(io_fiber, NULL);
    goc_shutdown();
    return 0;
}
```
