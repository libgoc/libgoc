[![CI](https://github.com/divs1210/libgoc/actions/workflows/ci.yml/badge.svg)](https://github.com/divs1210/libgoc/actions/workflows/ci.yml)
[![CD](https://github.com/divs1210/libgoc/actions/workflows/cd.yml/badge.svg)](https://github.com/divs1210/libgoc/actions/workflows/cd.yml)

![libgoc](assets/logo.png)

# libgoc
> A Go-style CSP concurrency runtime for C: threadpools, stackful coroutines, channels, select, async I/O, and garbage collection in one coherent API.

**libgoc** is a runtime library for C programs that want Go-style CSP concurrency backed by a managed memory environment. It is written in plain C for maximum reach and portability.

The library provides stackful coroutines ("fibers"), channels, a select primitive (`goc_alts`), timeout channels, a managed thread pool, and optional runtime telemetry (`goc_stats`). Boehm GC is a required dependency and is linked automatically.

**libgoc is built for:**

- **C developers** who want a Go-style concurrency runtime without switching to Go.
- **Language implementors** targeting C/C++ as a compilation target, or writing multithreaded interpreters.

**Dependencies:**

| | |
|---|---|
| `minicoro` | fiber suspend/resume (vendored MIT) |
| `libuv` | event loop, threads, timers, cross-thread wakeup |
| Boehm GC | garbage collection |
| `picohttpparser` | HTTP/1.1 request parser (vendored MIT); used by `goc_http`; disable with `-DLIBGOC_SERVER=OFF` |
| musl/TRE regex | POSIX ERE regex (vendored BSD-2-Clause); used by `goc_schema` |
| `yyjson` | JSON reader/writer (vendored MIT); used by `goc_json` |

**Pre-built static libraries** are available on the [Releases page](https://github.com/divs1210/libgoc/releases):
- Linux (x86-64)
- macOS (arm64)
- Windows (x86-64)

**API reference:**
- [Core API](./docs/GOC.md)
- [Async I/O](./docs/IO.md)
- [Async HTTP Client/Server](./docs/HTTP.md)
- [Dynamic Arrays](./docs/ARRAY.md)
- [Dictionaries](./docs/DICT.md)
- [Schemas](./docs/SCHEMA.md)
- [JSON](./docs/JSON.md)
- [Telemetry](./docs/TELEMETRY.md)

**Also see:**
- [Design Doc](./docs/DESIGN.md)
- [Benchmarks](/bench/README.md)

---

## Table of Contents

- [Examples](#examples)
  - [1. Ping-pong](#1-ping-pong)
  - [2. Custom thread pool with `goc_go_on`](#2-custom-thread-pool-with-goc_go_on)
  - [3. Using goc_malloc](#3-using-goc_malloc)
  - [4. JSON greeting over HTTP](#4-json-greeting-over-http)
- [Best Practices](#best-practices)
- [Building and Testing](#building-and-testing)
  - [Prerequisites](#prerequisites)
  - [macOS](#macos)
  - [Linux](#linux)
  - [Windows](#windows)
  - [Build types](#build-types)
  - [Code coverage](#code-coverage)
  - [Sanitizers](#sanitizers)

---

## Examples

### 1. Ping-pong

Two fibers exchange a message back and forth over a pair of unbuffered channels.
This is the canonical CSP "ping-pong" pattern — each fiber blocks on a take,
then immediately puts to wake the other side.

```c
#include "goc.h"
#include "goc_dict.h"
#include <stdio.h>

#define N_ROUNDS 5

static void player_fiber(void* arg) {
    goc_dict*    d    = arg;
    goc_chan*    recv = goc_dict_get(d, "recv", NULL);
    goc_chan*    send = goc_dict_get(d, "send", NULL);
    const char*  name = goc_dict_get(d, "name", NULL);

    goc_val_t* v;
    while ((v = goc_take(recv))->ok == GOC_OK) {
        int count = goc_unbox(int, v->val);
        printf("%s %d\n", name, count);
        if (count >= N_ROUNDS) {
            goc_close(send);
            return;
        }
        goc_put_boxed(int, send, count + 1);
    }
}

static void main_fiber(void* _) {
    goc_chan* a_to_b = goc_chan_make(0);
    goc_chan* b_to_a = goc_chan_make(0);

    goc_chan* done_ping = goc_go(player_fiber, goc_dict_of(
        {"recv", b_to_a}, {"send", a_to_b}, {"name", "ping"}
    ));
    goc_chan* done_pong = goc_go(player_fiber, goc_dict_of(
        {"recv", a_to_b}, {"send", b_to_a}, {"name", "pong"}
    ));

    /* Kick off the exchange with the first message. */
    goc_put_boxed(int, a_to_b, 1);

    /* Wait for both fibers to finish. */
    goc_take(done_ping);
    goc_take(done_pong);
}

int main(void) {
    goc_init();
    goc_go(main_fiber, NULL);
    goc_shutdown();
    return 0;
}
```

**What this example demonstrates:**

- `goc_chan_make(0)` — unbuffered channels enforce a synchronous rendezvous:
  each `goc_put` blocks until the other fiber calls `goc_take`, and vice versa.
- `goc_go` — spawns both player fibers on the current pool (or default pool
  when called outside fiber context) and returns a join channel that is closed
  automatically when the fiber returns.
- `goc_close` — when the round limit is reached the active fiber closes the
  forward channel, causing the partner's next `goc_take` to return
  `GOC_CLOSED` and exit its loop cleanly.
- `goc_dict_of(...)` — constructs a GC-managed key-value dict used here to pass
  multiple named arguments to a fiber in a single `void*`.
- `goc_put_boxed(T, ch, val)` / `goc_unbox(T, ptr)` — channels carry `void*`;
  boxing heap-allocates a scalar so it can be sent, unboxing dereferences it
  back to the original type on the receiving end.

---

### 2. Custom thread pool with `goc_go_on`

Use `goc_pool_make` when you need workloads isolated from the default pool —
for example, CPU-bound tasks that should not starve I/O fibers.

```c
#include "goc.h"
#include <stdio.h>

#define N_WORKERS 4

static void worker(void* arg) {
    int id = goc_unbox(int, arg);
    printf("worker %d done\n", id);
}

int main(void) {
    goc_init();

    goc_pool* pool = goc_pool_make(N_WORKERS);

    for (int i = 0; i < N_WORKERS; i++)
        goc_go_on(pool, worker, goc_box(int, i));

    /*
     * Destroy the CPU pool.
     * Optional, shown here for completeness.
     * All undestroyed pools are automatically cleaned up by goc_shutdown().
     */
    goc_pool_destroy(pool);

    goc_shutdown();
    return 0;
}
```

**What this example demonstrates:**

- `goc_pool_make` / `goc_pool_destroy` — creates and tears down a dedicated
  pool, isolated from the default pool started by `goc_init`.
- `goc_go_on` — pins each worker fiber to the pool.
- `goc_pool_destroy` blocks until all fibers on the pool finish, then frees resources.

---

### 3. Using goc_malloc

`goc_malloc` allocates memory on the Boehm GC heap. Allocations are collected
automatically when no longer reachable — no `free` is needed. Prefer the helper
macros `goc_new(T)` and `goc_new_n(T, n)` instead of
calling `goc_malloc(sizeof(T))` or `goc_malloc(n * sizeof(T))` directly.

```c
#include "goc.h"
#include <stdio.h>

typedef struct {
    double x;
    double y;
} Point;

int main(void) {
    goc_init();

    /* create a new Point object */
    Point* p = goc_new(Point);
    p->x = 1.5;
    p->y = 2.7;

    printf("Point(%f, %f)", p->x, p->y);

    /* create an array of Points */
    Point* ps = goc_new_n(Point, 5);
    Point* p0 = ps[0];

    goc_shutdown();
    return 0;
}
```

**A few things to keep in mind:**

- `goc_malloc` is a thin wrapper around `GC_malloc`. Memory is zero-initialised.
- `goc_new(T)` allocates a single `T` on the GC heap and returns a `T*`.
- `goc_new_n(T, n)` allocates an array of `n` values of type `T` on the GC heap and returns a `T*`.

---

### 4. JSON greeting over HTTP

A minimal HTTP example that demonstrates P11-style JSON request parsing and response serialisation. The client sends `{ "name": "Arjun" }`, and the server responds with `{ "response": "Hi, Arjun!" }`.

```c
#include "goc.h"
#include "goc_http.h"
#include "goc_json.h"
#include "goc_schema.h"
#include <stdio.h>

static goc_schema* request_schema;
static goc_schema* response_schema;

/* HTTP request handler */
static void greet_handler(goc_http_ctx_t* ctx) {
    goc_json_result req_r = goc_json_parse(goc_http_server_body_str(ctx));
    goc_dict* req = req_r.res;

    const char* name = goc_dict_get(req, "name", NULL);
    goc_dict* resp = goc_dict_of(
        {"response", goc_sprintf("Hi, %s!", name)}
    );

    goc_json_result out_r = goc_json_stringify(response_schema, resp);
    goc_http_server_respond(ctx, 200, "application/json", out_r.res);
}

static void main_fiber(void* _) {
    /* define schemas */
    request_schema = goc_schema_dict_of(
        {"name", goc_schema_str()}
    );
    response_schema = goc_schema_dict_of(
        {"response", goc_schema_str()}
    );

    /* make server */
    goc_http_server_opts_t* opts = goc_http_server_opts();
    goc_http_server* srv = goc_http_server_make(opts);
    goc_http_server_route(srv, "POST", "/greet", greet_handler);
    goc_chan* ready = goc_http_server_listen(srv, "127.0.0.1", 8080);
    goc_take(ready);

    /* send request */
    goc_dict* req_obj = goc_dict_of(
        {"name", "Arjun"}
    );
    goc_json_result req_json_r = goc_json_stringify(request_schema, req_obj);
    goc_chan* resp_ch = goc_http_post(
        "http://127.0.0.1:8080/greet",
        "application/json",
        req_json_r.res,
        goc_http_request_opts()
    );

    /* handle response */
    goc_http_response_t* resp = goc_take(resp_ch)->val;
    printf("server replied: %s\n", resp->body);

    /* shutdown server */
    goc_chan* close_ch = goc_http_server_close(srv);
    goc_take(close_ch);
}

int main(void) {
    goc_init();
    goc_go(main_fiber, NULL);
    goc_shutdown();
    return 0;
}
```

**What this example demonstrates:**

- `goc_json_parse` — parse an inbound JSON request body into a `goc_dict*`.
- `goc_json_stringify` — serialize a response object using a schema.
- `goc_http_server_route` and `goc_http_post` — wire a request/response roundtrip over HTTP.
- `goc_http_server_respond(..., "application/json", ...)` — send a JSON response with the correct MIME type.

---

## Best Practices

Used the right way, **libgoc** provides a runtime environment very similar to Go's.

**The blocking versions of take/put/alts are intended only for the initial setup in the `main` function, and should not be used otherwise.**

A typical program's main function should be like this:

```c
static void main_fiber(void* _) {
    /* 
     * User code comes here.
     * Since this is a fiber context,
     * async channel ops work here
     * and in all code reachable from here.
     */
}

int main(void) {
    goc_init();

    /* reify main thread as main fiber */
    goc_go(main_fiber, NULL);

    goc_shutdown();
    return 0;
}
```

## Building and Testing

<details>
<summary><i>expand / collapse</i></summary>

**Pre-built static libraries** are available on the [Releases page](https://github.com/divs1210/libgoc/releases).

libgoc ships with a comprehensive, phased test suite covering the full public API. See the [Testing section in the Design Doc](./docs/DESIGN.md#testing) for a breakdown of the test phases and what each one covers.

**`test.sh`** — Full build + test runner with optional watch mode:

```sh
./test.sh              # build and run all tests
WATCH=1 ./test.sh      # rebuild and rerun on any src/include/tests change
./test.sh -dbg 1       # enable verbose [GOC_DBG] output
```

Options: `-dbg <0|1>`, `-rp <0|1>` (SO_REUSEPORT for HTTP tests), `-vmem <0|1>`. Output is streamed to console and `test.log`. In watch mode, only previously-failing tests are rerun on the next change.

**`run_test_loop.sh`** — Stress a single test for flakiness detection:

```sh
./run_test_loop.sh tests/test_p06_thread_pool.c           # run up to 20 times
./run_test_loop.sh tests/test_p06_thread_pool.c -max-tries 100 -trace 1
```

Builds only the named target, runs it in a loop, and exits on the first failure. Each run is timestamped; log path is printed on exit.

### Prerequisites

| Dependency | macOS | Linux (Debian/Ubuntu) | Linux (Fedora/RHEL) | Windows |
|---|---|---|---|---|
| CMake ≥ 3.20 | `brew install cmake` | `apt install cmake` | `dnf install cmake` | MSYS2 UCRT64 (bundled) |
| libuv | `brew install libuv` | `apt install libuv1-dev` | `dnf install libuv-devel` | MSYS2 UCRT64 — see [Windows](#windows) |
| Boehm GC | `brew install bdw-gc` | source build (see below) | `dnf install gc-devel` | MSYS2 UCRT64 — see [Windows](#windows) |
| pkg-config | `brew install pkg-config` | `apt install pkg-config` | `dnf install pkgconfig` | MSYS2 UCRT64 (bundled) |
| minicoro | vendored (`vendor/minicoro/`); instantiated via `src/minicoro.c` |

A C11 compiler is required: GCC or Clang on Linux/macOS; MinGW-w64 GCC via MSYS2 UCRT64 on Windows.

libgoc is built to link statically against `libuv` and Boehm GC. Ensure static versions of those dependencies are available to `pkg-config` before configuring.

---

### macOS

```sh
# 1. Install dependencies (Homebrew)
brew install cmake libuv bdw-gc pkg-config

# Homebrew's bdw-gc does not ship a bdw-gc-threaded.pc pkg-config alias.
# Create it once in the global Homebrew pkgconfig directory:
PKGDIR="$(brew --prefix)/lib/pkgconfig"
[ -f "$PKGDIR/bdw-gc-threaded.pc" ] || cp "$PKGDIR/bdw-gc.pc" "$PKGDIR/bdw-gc-threaded.pc"

# 2. Configure
export PKG_CONFIG_ALL_STATIC=1
cmake -B build -DLIBGOC_STATIC_DEPENDENCIES=ON

# 3. Build
cmake --build build

# 4. Run tests
ctest --test-dir build --output-on-failure

# Or run a single phase directly for full output
./build/test_p01_foundation
```

---

### Linux

```sh
# 1. Install dependencies (Debian/Ubuntu shown; see table above for RPM)
sudo apt update
sudo apt install cmake libuv1-dev libatomic-ops-dev pkg-config build-essential

# Ubuntu's libgc-dev is NOT compiled with --enable-threads, which libgoc requires.
# GC_allow_register_threads is required for libgoc's goc_thread_create/
# goc_thread_join wrappers; the system package can crash at runtime.
# Build Boehm GC from source instead:
wget https://github.com/ivmai/bdwgc/releases/download/v8.2.6/gc-8.2.6.tar.gz
tar xf gc-8.2.6.tar.gz && cd gc-8.2.6
./configure --enable-threads=posix --enable-thread-local-alloc --disable-shared --enable-static --prefix=/usr/local
make -j$(nproc) && sudo make install && sudo ldconfig && cd ..

# The source build does not always generate a bdw-gc-threaded.pc pkg-config alias.
# Create it manually if it is missing:
if [ ! -f /usr/local/lib/pkgconfig/bdw-gc-threaded.pc ]; then
    sudo ln -s /usr/local/lib/pkgconfig/bdw-gc.pc /usr/local/lib/pkgconfig/bdw-gc-threaded.pc
fi

# Ensure pkg-config searches /usr/local (not on the default path on all distros):
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
# To make this permanent:
# echo 'export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH' >> ~/.bashrc

# 2. Configure
export PKG_CONFIG_ALL_STATIC=1
cmake -B build -DLIBGOC_STATIC_DEPENDENCIES=ON

# 3. Build
cmake --build build

# 4. Run tests
ctest --test-dir build --output-on-failure

# Or run a single phase directly
./build/test_p01_foundation
```

---

### Windows

libgoc uses libuv thread primitives (`uv_thread_t`, etc.) and C11 atomics via `<stdatomic.h>` (`_Atomic`, `atomic_*`). MSVC builds are still not supported (notably due to bdwgc/toolchain constraints, including vcpkg's Win32-threads build), so the recommended Windows setup remains **MSYS2/MinGW-w64 (UCRT64)**.

```sh
# 1. Install MSYS2 from https://www.msys2.org/, then in a UCRT64 shell:
pacman -S mingw-w64-ucrt-x86_64-gcc \
          mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-libuv \
          mingw-w64-ucrt-x86_64-gc \
          mingw-w64-ucrt-x86_64-pkg-config

# 2. Create the bdw-gc-threaded pkg-config alias if it is missing
PKGDIR="/ucrt64/lib/pkgconfig"
[ -f "$PKGDIR/bdw-gc-threaded.pc" ] || cp "$PKGDIR/bdw-gc.pc" "$PKGDIR/bdw-gc-threaded.pc"

# 3. Configure and build everything (library + tests)
export PKG_CONFIG_ALL_STATIC=1
cmake -B build -DLIBGOC_STATIC_DEPENDENCIES=ON
cmake --build build --parallel $(nproc)

# 4. Run tests
ctest --test-dir build --output-on-failure
```

> **Tests:** Phases P1–P7 and P9 run normally on Windows. Phase 8 (safety tests) requires `fork()`/`waitpid()` to isolate processes that call `abort()` — these POSIX APIs are not available in MinGW. The P8 test binary builds successfully but all 11 tests report `skip` at runtime.

---

### Build types

```sh
# Debug (no optimisation, debug symbols)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Release
cmake -B build -DCMAKE_BUILD_TYPE=Release

# RelWithDebInfo (default)
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

---

### Stack allocator

```sh
# Default: canary-protected stacks (recommended, portable)
cmake -B build

# Enable virtual memory allocator (dynamic stack growth)
cmake -B build -DLIBGOC_VMEM=ON
```

The default fiber stack size can be set at build time:

```sh
cmake -B build -DLIBGOC_STACK_SIZE=131072   # 128 KB
```

---

### Installation and pkg-config

`libgoc` is installed as a static archive plus headers. The install step writes a `libgoc.pc` pkg-config file to `<prefix>/lib/pkgconfig/`, so downstream projects can locate and link `libgoc` without knowing its install prefix.

```sh
cmake -B build
cmake --build build
sudo cmake --install build   # installs goc.h, goc_io.h, goc_array.h, libgoc.a, and libgoc.pc
```

```sh
# Compile and link a consumer with pkg-config
cc $(pkg-config --cflags libgoc) my_app.c $(pkg-config --libs libgoc) -o my_app
```

In a CMake-based consumer, use `pkg_check_modules` in the same way as libgoc itself uses it for libuv:

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGOC REQUIRED IMPORTED_TARGET libgoc)
target_link_libraries(my_target PRIVATE PkgConfig::LIBGOC)
```

---

### Code coverage

Code coverage instrumentation is opt-in via `-DLIBGOC_COVERAGE=ON`. It requires GCC or Clang and uses `gcov`-compatible `.gcda`/`.gcno` files. If `lcov` and `genhtml` are found, a `coverage` build target is also registered that runs the test suite and produces a self-contained HTML report.

**Install lcov**

| Platform | Command |
|---|---|
| macOS | `brew install lcov` |
| Debian/Ubuntu | `apt install lcov` |
| Fedora/RHEL | `dnf install lcov` |

**Configure and build**

```sh
# Coverage builds should use Debug to avoid optimisation hiding branches
cmake -B build-cov \
      -DCMAKE_BUILD_TYPE=Debug \
      -DLIBGOC_COVERAGE=ON
cmake --build build-cov
```

**Generate the HTML report**

```sh
cmake --build build-cov --target coverage
# Report written to: build-cov/coverage_html/index.html
open build-cov/coverage_html/index.html   # macOS
xdg-open build-cov/coverage_html/index.html  # Linux
```

The `coverage` target runs `ctest` internally, so there is no need to invoke the test binary separately. The final report includes branch coverage and filters out system headers and build-system generated files.

> **Note:** Coverage and sanitizer builds are mutually exclusive — configure them in separate build directories. Coverage is also incompatible with `-DCMAKE_BUILD_TYPE=Release` optimisation levels that inline or eliminate branches.

---

### Sanitizers

AddressSanitizer and ThreadSanitizer builds are available as opt-in targets.

```sh
# AddressSanitizer
cmake -B build-asan -DLIBGOC_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure

# ThreadSanitizer
cmake -B build-tsan -DLIBGOC_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

> **Note:** ASAN and TSAN are mutually exclusive — configure them in separate build directories.

</details>

---

*Copyright (c) Divyansh Prakash | [MIT License](./LICENSE)*
