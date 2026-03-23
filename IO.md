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
  - [Stream I/O result types](#stream-io-result-types)
  - [UDP result types](#udp-result-types)
  - [File system result types](#file-system-result-types)
  - [DNS result types](#dns-result-types)
- [1. Stream I/O (TCP, Pipes, TTY)](#1-stream-io-tcp-pipes-tty)
- [2. UDP (Datagrams)](#2-udp-datagrams)
- [3. File System Operations](#3-file-system-operations)
- [4. DNS & Resolution](#4-dns--resolution)

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
is synchronous and takes no callback, so it is outside the scope of `goc_io`.
Use raw libuv functions with `goc_scheduler()` as the loop argument:

```c
uv_tcp_t* tcp = goc_malloc(sizeof(uv_tcp_t));
uv_tcp_init(goc_scheduler(), tcp);
```

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

Scalar-returning operations (write, connect, open, read, etc.) use the raw
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

### Result types for composite operations

Single-result operations (write, connect, open, read, etc.) return scalars encoded in the channel:

| Operation | Channel delivers |
|---|---|
| `goc_io_write`, `goc_io_write2`, `goc_io_shutdown_stream`, `goc_io_tcp_connect`, `goc_io_pipe_connect`, `goc_io_udp_send`, `goc_io_fs_close`, `goc_io_fs_unlink`, `goc_io_fs_rename` | `goc_box_int(status)` — 0 on success, negative libuv error |
| `goc_io_fs_open` | `goc_box_int(fd)` — fd >= 0 on success, negative libuv error |
| `goc_io_fs_read`, `goc_io_fs_write`, `goc_io_fs_sendfile` | `goc_box_int(result)` — bytes >= 0 on success, negative libuv error |

Decode scalar channel values with `goc_unbox_int(...)`.

Composite result types (channel delivers a GC-managed pointer):

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

/* goc_io_fs_stat_t — result of goc_io_fs_stat.
 * ok == GOC_IO_OK: success (statbuf populated).  ok == GOC_IO_ERR: failure. */
typedef struct {
    goc_io_status_t ok;
    uv_stat_t    statbuf;
} goc_io_fs_stat_t;

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

---

## 2. UDP (Datagrams)

| Function | Signature | Description |
|---|---|---|
| `goc_io_udp_send` | `goc_chan* goc_io_udp_send(uv_udp_t* handle, const uv_buf_t bufs[], unsigned int nbufs, const struct sockaddr* addr)` | Initiate an async UDP send; return result channel delivering `goc_box_int(status)`. 0 on success, negative libuv error on failure. |
| `goc_io_udp_recv_start` | `goc_chan* goc_io_udp_recv_start(uv_udp_t* handle)` | Begin receiving UDP datagrams. Returns a channel delivering `goc_io_udp_recv_t*` values, one per datagram. Channel is closed on unrecoverable error. Call `goc_io_udp_recv_stop()` to stop. |
| `goc_io_udp_recv_stop` | `int goc_io_udp_recv_stop(uv_udp_t* handle)` | Stop receiving UDP datagrams and close the receive channel. Returns 0; takes effect asynchronously. |

---

## 3. File System Operations

All file-system functions are safe to call from any context (fiber or OS thread).

| Function | Signature | Description |
|---|---|---|
| `goc_io_fs_open` | `goc_chan* goc_io_fs_open(const char* path, int flags, int mode)` | Async file open; channel delivers `goc_box_int(fd)`. fd >= 0 on success, negative libuv error on failure. Use `UV_FS_O_*` flags (e.g. `UV_FS_O_RDONLY`, `UV_FS_O_WRONLY \| UV_FS_O_CREAT`). |
| `goc_io_fs_close` | `goc_chan* goc_io_fs_close(uv_file file)` | Async file close; channel delivers `goc_box_int(status)`. 0 on success, negative libuv error on failure. |
| `goc_io_fs_read` | `goc_chan* goc_io_fs_read(uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset)` | Async file read; channel delivers `goc_box_int(result)`. Bytes read >= 0 on success, negative libuv error on failure. Pass `offset == -1` to use the current file position. |
| `goc_io_fs_write` | `goc_chan* goc_io_fs_write(uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset)` | Async file write; channel delivers `goc_box_int(result)`. Bytes written >= 0 on success, negative libuv error on failure. |
| `goc_io_fs_unlink` | `goc_chan* goc_io_fs_unlink(const char* path)` | Async file deletion; channel delivers `goc_box_int(status)`. 0 on success, negative libuv error on failure. |
| `goc_io_fs_stat` | `goc_chan* goc_io_fs_stat(const char* path)` | Async file stat; channel delivers `goc_io_fs_stat_t*`. |
| `goc_io_fs_rename` | `goc_chan* goc_io_fs_rename(const char* path, const char* new_path)` | Async file rename; channel delivers `goc_box_int(status)`. 0 on success, negative libuv error on failure. |
| `goc_io_fs_sendfile` | `goc_chan* goc_io_fs_sendfile(uv_file out_fd, uv_file in_fd, int64_t in_offset, size_t length)` | Async zero-copy file transfer; channel delivers `goc_box_int(result)`. Bytes transferred >= 0 on success, negative libuv error on failure. |

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
    char data[] = "hello, libgoc\n";
    uv_buf_t buf = uv_buf_init(data, sizeof(data) - 1);
    goc_val_t* vw = goc_take(goc_io_fs_write(fd, &buf, 1, 0));
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

## 4. DNS & Resolution

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
