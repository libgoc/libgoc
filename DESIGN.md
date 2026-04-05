# `libgoc` — Design Reference
> A Go-style CSP concurrency runtime for C: threadpools, stackful coroutines, channels, select, async I/O, and garbage collection in one coherent API.

- Stackful coroutines. Type-erased. Boehm GC integrated.
- libuv for I/O, timers, and cross-thread wakeup.
- POSIX (Linux/macOS) and Windows.

---

## Table of Contents

1. [Dependencies](#dependencies)
2. [Repository Structure](#repository-structure)
3. [Build System Setup](#build-system-setup)
4. [Thread Model](#thread-model)
5. [libuv Role](#libuv-role)
6. [Boehm GC Integration](#boehm-gc-integration) · [One-Time Initialization Constraint](#one-time-initialization-constraint)
7. [Data Structures](#data-structures)
8. [Fiber Mechanics](#fiber-mechanics) · [Join Channel](#join-channel)
9. [minicoro Limitations](#minicoro-limitations)
10. [Channel Operation Flow](#channel-operation-flow)
11. [Thread Pool](#thread-pool)
12. [Cross-Thread Wakeup via `uv_async_t`](#cross-thread-wakeup-via-uv_async_t)
13. [MPSC Callback Queue](#mpsc-callback-queue)
14. [`goc_timeout` — libuv Timer](#goc_timeout--libuv-timer)
15. [`goc_alts`](#goc_alts)
16. [Public API](#public-api)
17. [Async I/O Wrappers (`goc_io`)](#async-io-wrappers-goc_io)
18. [Telemetry (`goc_stats`)](#telemetry-goc_stats)
19. [Initialization Sequence](#initialization-sequence)
20. [Shutdown Sequence](#shutdown-sequence)
21. [Testing](#testing)
22. [CI/CD](#cicd)
23. [HTTP Server (`goc_http`)](#http-server-goc_http)

> **Dynamic Array:** For `goc_array` design rationale and full API see [ARRAY.md](./ARRAY.md).
> **Telemetry:** For full `goc_stats` API and event reference see [TELEMETRY.md](./TELEMETRY.md).
> **HTTP Server:** For full `goc_http` design and API rationale see [HTTP.md](./HTTP.md).

---

## Dependencies

| | |
|---|---|
| `minicoro` | fiber suspend/resume (cross-platform; POSIX and Windows) |
| `libuv` | event loop, timers, cross-thread signalling |
| Boehm GC (bdw-gc) | garbage collection; **must be built with `--enable-threads`**; hard dependency, initialised by `goc_init`; thread pool workers and the uv loop thread are registered with the collector on creation and unregistered on exit |
| `picohttpparser` | HTTP/1.1 request parser (vendored MIT, `vendor/picohttpparser/`); used by `goc_http`; compiled in by default; excluded with `-DLIBGOC_SERVER=OFF` |

---

## Repository Structure

**Not a header-only library.** libgoc requires compiled C source modules:

```
libgoc/
├── include/
│   ├── goc.h              # Public API header
│   ├── goc_io.h           # Async I/O wrappers public API (separate include)
│   ├── goc_array.h        # Dynamic array public API
│   ├── goc_stats.h        # Telemetry public API (opt-in; requires GOC_ENABLE_STATS)
│   └── goc_http.h       # HTTP server public API (separate include)
├── src/
│   ├── alts.c             # goc_alts, goc_alts_sync
│   ├── timeout.c          # goc_timeout
│   ├── gc.c               # Boehm GC initialization, goc_malloc, goc_realloc
│   ├── loop.c             # libuv event loop thread; MPSC callback queue
│   ├── pool.c             # Thread pool workers
│   ├── fiber.c            # minicoro fiber mechanics
│   ├── channel.c          # Channel operations
│   ├── mutex.c            # RW mutexes (channel-backed lock handles)
│   ├── goc_array.c        # Dynamic array (goc_array)
│   ├── goc_io.c           # Async I/O wrappers (libuv; see goc_io.h)
│   ├── goc_stats.c        # Telemetry implementation (compiled only when GOC_ENABLE_STATS is set)
│   ├── goc_http.c       # HTTP/1.1 server and client (picohttpparser + goc_io; see goc_http.h)
│   ├── minicoro.c         # Instantiates minicoro (defines MINICORO_IMPL)
│   ├── internal.h         # Internal types, helpers, and cross-module declarations
│   ├── chan_type.h         # Authoritative struct goc_chan definition (included where concrete channel fields are accessed)
│   ├── channel_internal.h  # Channel-only internals and inline channel helpers
│   └── config.h           # Build configuration constants (thresholds, etc.)
├── tests/
│   ├── test_harness.h               # Shared harness macros + SIGSEGV/SIGABRT crash handler
│   ├── test_harness.c               # Shared harness implementation (compiled into every test binary)
│   ├── test_p01_foundation.c        # Phase 1  — Foundation
│   ├── test_goc_array.c             # Component — goc_array dynamic array
│   ├── test_goc_stats.c             # Component — goc_stats telemetry
│   ├── test_p11_http.c              # Phase 11 — HTTP server/client (goc_http)
│   ├── test_p02_channels_fibers.c   # Phase 2  — Channels and fiber launch
│   ├── test_p03_channel_io.c        # Phase 3  — Channel I/O
│   ├── test_p04_callbacks.c         # Phase 4  — Callbacks
│   ├── test_p05_select_timeout.c    # Phase 5  — Select and timeout
│   ├── test_p06_thread_pool.c       # Phase 6  — Thread pool
│   ├── test_p07_integration.c       # Phase 7  — Integration
│   ├── test_p08_safety.c            # Phase 8  — Safety and crash behaviour
│   ├── test_p09_mutexes.c           # Phase 9  — RW mutexes
│   └── test_p10_io.c                # Phase 10 — Async I/O wrappers
├── bench/                           # Benchmarks (see bench/README.md)
│   ├── go/                          # Go benchmark sources
│   ├── libgoc/                      # C benchmark sources
│   ├── go.mod
│   └── README.md
├── vendor/
│   ├── minicoro/
│   │   └── minicoro.h             # Vendored header — fiber suspend/resume (header-only)
│   └── picohttpparser/
│       ├── picohttpparser.h       # Vendored header — fast HTTP/1.1 request parser (MIT)
│       └── picohttpparser.c       # Vendored implementation
├── CMakeLists.txt         # Build system: libgoc static lib + test binary
├── libgoc.pc.in           # pkg-config template; expanded by CMake at configure time
├── README.md
├── DESIGN.md              # This document
├── ARRAY.md               # Dynamic array design and API reference
├── TELEMETRY.md           # goc_stats async telemetry system
├── HTTP.md                # HTTP server API and design reference
├── OPTIMIZATION.md        # Prioritized optimization roadmap and benchmark signals
├── TODO.md                # Planned future work
└── LICENSE
```

Benchmarks live in `bench/` and are documented in [bench/README.md](./bench/README.md).

minicoro is a single-header library vendored under `vendor/minicoro/`. It requires exactly one translation unit to instantiate its implementation: `src/minicoro.c` defines `MINICORO_IMPL` before including `minicoro.h`. All other files include `minicoro.h` without defining `MINICORO_IMPL`.

`src/minicoro.c` is compiled as an isolated translation unit with minicoro-specific flags (see [minicoro Limitations](#minicoro-limitations)). CMake enforces:

- always: `-UGC_THREADS -UGC_PTHREADS -DMCO_ZERO_MEMORY`
- when `-DLIBGOC_VMEM=ON`: additionally `-DMCO_USE_VMEM_ALLOCATOR`

This is implemented via `set_source_files_properties(src/minicoro.c PROPERTIES COMPILE_OPTIONS ...)` in `CMakeLists.txt`.

---

## Build System Setup

The project uses CMake (≥ 3.20). `CMakeLists.txt` defines the following primary targets:

| Target | Type | Description |
|---|---|---|
| `goc` | static library | All `src/*.c` modules; always built |
| `goc_shared` | shared library | Same sources as `goc`; built only with `-DLIBGOC_SHARED=ON`; output name `libgoc.so` / `.dylib` / `.dll` |
| `test_p01_foundation` … `test_p10_io` | executables | One per phase, discovered via `file(GLOB tests/test_p*.c)`; each linked against the active `goc` variant + libuv + Boehm GC |
| `test_goc_array` | executable | Component test for `goc_array`; discovered via `file(GLOB tests/test_goc_*.c)` |
| `test_goc_stats` | executable | Component test for `goc_stats`; always compiled with `GOC_ENABLE_STATS` and `src/goc_stats.c` added directly, regardless of the `GOC_ENABLE_STATS` CMake option |
| `test_p11_http` | executable | Phase 11 tests for `goc_http` (server lifecycle, routing, handlers, middleware, HTTP client, security, ping-pong integration); compiled only when `LIBGOC_SERVER=ON` (default) |

A CMake function `goc_configure_target(<target>)` centralises the options shared by every library variant: `PUBLIC` include path `include/`, `PRIVATE` paths `src/`, `vendor/minicoro/`, and (when `LIBGOC_SERVER` is ON) `vendor/picohttpparser/`, compile definition `GC_THREADS`, and link libraries `PkgConfig::LIBUV` and `PkgConfig::BDWGC`. All library targets (`goc`, `goc_shared`, `goc_asan`, `goc_tsan`) are configured through this function.

When `-DLIBGOC_VMEM=ON` is passed, `LIBGOC_VMEM_ENABLED` is added as a `PRIVATE` compile definition on the `goc` library target **and** on every per-phase test executable. This ensures that `src/internal.h`'s canary macros are disabled and that `test_p08_safety.c` detects the vmem build at compile time (P8.1 uses `#ifdef LIBGOC_VMEM_ENABLED` to skip the canary-abort test in vmem builds, where the canary is a no-op). By default (canary mode), `LIBGOC_VMEM_ENABLED` is **not** defined and canary protection is active.

Dependencies are resolved via `pkg-config` (libuv as `libuv`, Boehm GC as `bdw-gc-threaded` — **no fallback**; configure fails loudly if the threaded variant is absent). minicoro is instantiated via `src/minicoro.c` (which defines `MINICORO_IMPL`) and its header is available to all targets via `target_include_directories` pointing at `vendor/minicoro/`.

> **Boehm GC thread registration:** libgoc compiles with `-DGC_THREADS` and requires a Boehm GC built with `--enable-threads` (the `bdw-gc-threaded` pkg-config module). Linking against a non-threaded build causes `GC_INIT()` to malfunction or segfault at runtime; CMake will fail at configure time if the threaded variant is absent. See `README.md` for per-platform installation instructions. All threads are created via `goc_thread_create` (declared in `src/internal.h`, implemented in `src/gc.c`). It wraps `uv_thread_create` with a trampoline (`goc_thread_trampoline`) that calls `GC_register_my_thread` at startup and `GC_unregister_my_thread` at exit on all platforms. `GC_allow_register_threads()` must have been called first — `goc_init()` does this.

Named constants defined in `config.h`:
- `GOC_DEAD_COUNT_THRESHOLD 8`
- `GOC_ALTS_STACK_THRESHOLD 8` (maximum number of `goc_alts` arms for which the channel-pointer scratch buffer is stack-allocated rather than `malloc`-allocated; avoids a heap allocation for the common case of a small select)
- `GOC_DEFAULT_LIVE_FIBER_MEMORY_FACTOR 0.6` (safety/throughput factor in the default live-fiber admission cap formula `floor(factor × available_memory / fiber_stack_size)`; the 0.6 value reserves ~40% memory headroom for GC and runtime overhead; overridable at runtime via `GOC_MAX_LIVE_FIBERS`)

Optional opt-in flags, each requiring a **separate build directory**:

| CMake flag | Extra target(s) | Notes |
|---|---|---|
| `-DLIBGOC_SHARED=ON` | `goc_shared` | Shared library variant; installed by `install(TARGETS goc_shared …)` |
| `-DLIBGOC_ASAN=ON` | `goc_asan` | AddressSanitizer; per-phase test executables link against `goc_asan`; mutually exclusive with TSAN and COVERAGE |
| `-DLIBGOC_TSAN=ON` | `goc_tsan` | ThreadSanitizer; per-phase test executables link against `goc_tsan`; mutually exclusive with ASAN and COVERAGE |
| `-DLIBGOC_COVERAGE=ON` | `coverage` target (if lcov/genhtml found) | Instruments `goc` with `--coverage`; runs ctest then produces `coverage_html/index.html`; mutually exclusive with ASAN and TSAN |
| `-DGOC_ENABLE_STATS=ON` | *(none)* | Compiles `src/goc_stats.c` into the library and defines `GOC_ENABLE_STATS` on all targets; without this flag the telemetry macros are no-ops and `goc_stats.c` is excluded from `libgoc.a`. `test_goc_stats` always compiles `goc_stats.c` directly regardless of this flag. |
| `-DLIBGOC_SERVER=OFF` | *(none)* | Excludes `src/goc_http.c` and `vendor/picohttpparser/picohttpparser.c` from the library. The HTTP server and client APIs are unavailable. Default is **ON**; this flag is an opt-out. |

ASAN and TSAN each compile a separate instrumented copy of the `goc` static library (`goc_asan` / `goc_tsan`) so that sanitizer flags propagate through all object files. When either sanitizer flag is active the per-phase test executables link against the instrumented variant instead of `goc`. Configuring ASAN and TSAN together, or either sanitizer with COVERAGE, in the same directory is a CMake fatal error.

**Install rules** (`cmake --install`): `libgoc.a` is installed to `${CMAKE_INSTALL_LIBDIR}`, `include/goc.h` to `${CMAKE_INSTALL_INCLUDEDIR}`, and (when `-DLIBGOC_SHARED=ON`) the shared library to `${CMAKE_INSTALL_LIBDIR}` / `${CMAKE_INSTALL_BINDIR}`. A `pkg-config` file is generated from `libgoc.pc.in` at configure time and installed to `${CMAKE_INSTALL_LIBDIR}/pkgconfig/libgoc.pc`.

> **Static archive link order:** The internal declarations are split so channel-only helpers live in `src/channel_internal.h` while runtime-wide declarations remain in `src/internal.h`. With this layout, test executables link directly against the selected `goc` target via `target_link_libraries` without Linux-specific archive-rescan linker options.

---

## Thread Model

```
┌─────────────────────┐     uv_async_send()     ┌─────────────────┐
│   pool thread 0     │ ──────────────────────► │                 │
│   pool thread 1     │                          │  uv loop thread │
│   pool thread 2     │ ◄────────────────────── │                 │
│        ...          │   post fiber to run queue└─────────────────┘
└─────────────────────┘
        ▲
   fibers run here
   goc_take / goc_put
   channel wakeup
```

Pool threads execute fibers. The uv loop thread never resumes fibers directly. Instead:

- pool threads call `uv_async_send` only to wake the **uv loop thread** so it can drain callback work,
- loop-thread callbacks such as timers and async handlers interact with channels,
- channel wakeups re-enqueue fibers onto pool workers via `post_to_run_queue`.

The pool threads and the event loop thread are **separate**. Fibers run on pool threads. The event loop thread only drives timers and dispatches wakeups — it never runs fiber code.

---

## libuv Role

libuv owns **one thing**: the event loop thread.

| libuv primitive | used for |
|---|---|
| `uv_loop_t` | single event loop, runs on its own thread |
| `uv_async_t` | wake the loop from other threads so it can drain callback/shutdown work |
| `uv_timer_t` | `goc_timeout` implementation |

**All libuv handles and internal context structs must be GC-allocated (`goc_malloc`) and pinned in the `live_uv_handles` root array** before being handed to libuv. libuv holds internal references to handles until `uv_close` completes; those references are not visible to the GC. Without pinning the GC would collect the object prematurely, causing use-after-free inside the event loop. Internal code uses `gc_handle_register`/`gc_handle_unregister` directly; user code uses the public wrappers `goc_io_handle_register` / `goc_io_handle_unregister` / `goc_io_handle_close`.

User-created handles registered on the loop via `goc_scheduler()` follow the same rule: allocate with `goc_malloc`, register with `goc_io_handle_register`, and close with `goc_io_handle_close`.

### User I/O Callbacks and the uv Loop Thread

libuv invokes **all handle callbacks** (read callbacks, connection callbacks, timer callbacks, etc.) on the **uv loop thread**. Fiber code never runs on this thread. This means:

- You **must not** call `goc_take` or `goc_put` from a libuv callback. Both functions may block by suspending a fiber, which is illegal on the loop thread.
- You **must not** call `goc_take_sync` or `goc_put_sync` from a libuv callback. These block the calling OS thread, which would deadlock the entire event loop.

**Correct pattern — hand off with `goc_put_cb` / `goc_take_cb`:**

```c
/* libuv read callback — runs on the uv loop thread */
static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    if (nread <= 0) return;

    my_msg_t* msg = goc_malloc(sizeof(my_msg_t));
    memcpy(msg->data, buf->base, nread);
    msg->len = nread;

    /* goc_put_cb returns immediately; cb fires on the loop thread once a taker is ready. */
    goc_put_cb(data_ch, msg, on_put_done, msg);
}

static void on_put_done(goc_status_t ok, void* ud) {
    (void)ok; (void)ud;  /* GC-allocated; no free needed even if channel closed */
}
```

Conversely, to pull a value from a channel from loop-thread code:

```c
goc_take_cb(result_ch, on_value_ready, my_ud);

static void on_value_ready(void* val, goc_status_t ok, void* ud) {
    /* also runs on the uv loop thread — do not block */
    process(val);
}
```

If the fiber side needs to produce or consume multiple values, launch a fiber with `goc_go` and use plain `goc_put`/`goc_take` there. Keep loop-thread callbacks short and non-blocking.

---

## Boehm GC Integration

Boehm GC (bdw-gc) is a **required link-time dependency**. It is not optional and requires no configuration from the caller.

`goc_init` must be **the first call in `main()`**, before any other library or application code that could trigger GC allocation. It calls `GC_INIT()` unconditionally as its very first operation, then installs the fiber-root push callback, then calls `GC_allow_register_threads()`. `GC_INIT()` performs all one-time GC setup, and `GC_allow_register_threads()` enables multi-thread stack registration. **Callers must not call these functions themselves.**

Pool workers and the uv loop thread are created via `goc_thread_create` on all platforms. It wraps `uv_thread_create` with a trampoline that calls `GC_register_my_thread` at startup and `GC_unregister_my_thread` at exit, ensuring every thread is visible to the collector. `GC_allow_register_threads()` is called by `goc_init()` before any threads are spawned.

**`goc_init` must be called exactly once, as the very first call in `main()`, before any other library or application code that could trigger GC allocation. Calling it more than once is undefined behaviour.**

`goc_malloc(n)` is the public allocator. It is a thin wrapper around `GC_malloc`. Memory is zero-initialised and collected automatically when no longer reachable — no `free` is required or permitted.

`goc_sprintf(fmt, ...)` is a GC-heap-aware `sprintf`. It calls `vsnprintf` twice (once to measure, once to fill) and returns a null-terminated string allocated via `goc_malloc`. The caller must not `free` the result.

**libuv handles and internal context/dispatch structs are `goc_malloc`-allocated and pinned via `gc_handle_register`** — see [libuv Role](#libuv-role). The library uses plain `malloc` only for objects whose lifetime is explicitly managed and that do not need to participate in GC traversal (e.g. channel mutex internals, injector queue nodes).

Most runtime objects that should participate in GC traversal (channels, fibers, parked entries, callback-queue nodes) are allocated via `goc_malloc`. Scheduler storage has a few intentional exceptions: work-stealing deque ring buffers use `GC_malloc_uncollectable` so queued `goc_entry*` values remain visible to the collector, and injector queue nodes use plain `malloc`/`free` so arbitrary producer threads do not need to perform GC allocations.

---

## Data Structures

Most GC-managed structs are allocated via `goc_malloc` — on the GC heap, so pointers inside them are visible to the collector automatically.

**Exceptions (plain `malloc`):**
- channel mutex internals (`uv_mutex_t*` inside `goc_chan`), destroyed by `goc_shutdown`;
- work-stealing deque ring buffers use `GC_malloc_uncollectable`;
- injector queue nodes use plain `malloc`/`free`.

See [libuv Role](#libuv-role) above.

### Portable Semaphore (`goc_sync_t`)

```c
typedef struct {
    uv_mutex_t mtx;
    uv_cond_t  cond;
    int        ready;
} goc_sync_t;
```

A single-use binary semaphore backed by a `uv_mutex_t` + `uv_cond_t` pair. Used by the `GOC_SYNC` blocking path in `goc_take_sync`, `goc_put_sync`, and `goc_alts_sync` instead of `sem_t`, which is non-functional for unnamed POSIX semaphores on macOS (`sem_init` returns `ENOSYS`). `goc_sync_post` may be called before or after `goc_sync_wait`; if post fires first, wait returns immediately. Defined in `src/internal.h`.

### Value (`goc_val_t`)

```c
typedef enum {
    GOC_EMPTY  = -1,  /* channel open but no value available (goc_take_try only) */
    GOC_CLOSED =  0,  /* channel was closed */
    GOC_OK     =  1,  /* value delivered successfully */
} goc_status_t;

typedef struct {
    void*        val;
    goc_status_t ok;
} goc_val_t;
```

Returned by `goc_take`, `goc_take_sync`, `goc_take_try`, and the `value` field of `goc_alts_result_t`. `ok==GOC_CLOSED` always means the channel was closed, regardless of `val`. `ok==GOC_EMPTY` is returned only by `goc_take_try` when the channel is open but has no value immediately available.

### Channel (`goc_chan`)

The full struct definition lives in `src/chan_type.h` — the single authoritative location. Channel-facing code reaches it via `channel_internal.h`, and lifecycle teardown code that touches channel internals includes it directly (for example, `gc.c` to destroy `ch->lock` during shutdown). Code that does not need concrete channel fields continues to treat `goc_chan` as opaque via `goc.h`.

```c
/* src/chan_type.h */
struct goc_chan {
    void**       buf;         /* ring buffer, GC-allocated */
    size_t       buf_size;
    size_t       buf_head;
    size_t       buf_count;
    goc_entry*   takers;      /* head of parked-taker linked list */
    goc_entry*   takers_tail; /* tail of takers list; NULL = unknown (invalidated by removal) */
    goc_entry*   putters;     /* head of parked-putter linked list */
    goc_entry*   putters_tail;/* tail of putters list; NULL = unknown */
    uv_mutex_t*  lock;        /* malloc-allocated; destroyed by goc_shutdown after pool drain */
    int          closed;
    size_t       dead_count;  /* cancelled entries still physically on takers/putters lists */
    _Atomic int  close_guard; /* 0 = open; CAS 0→1 to become the sole teardown owner */
    void       (*on_close)(void*);  /* optional callback invoked when the channel is closed */
    void*        on_close_ud;       /* user data forwarded to on_close */
};
```

`lock` is a `uv_mutex_t*` allocated with `malloc` in `goc_chan_make` and initialised with `uv_mutex_init`. **Mutex teardown is deferred — it is not performed inside `goc_close`.** The mutex is destroyed and freed by `goc_shutdown` in Step 3, after all pool threads and the event loop thread have been joined so no code path can ever call `uv_mutex_lock(ch->lock)` again. See [Shutdown Sequence](#shutdown-sequence).

**`goc_close` uses an atomic CAS on `close_guard` to serialise concurrent closers:**

1. **CAS:** attempt to flip `close_guard` from `0` to `1` with `acq_rel` ordering.
2. **Loser path:** if the CAS fails, another caller already owns teardown — return immediately. No pointer is read, no lock is attempted.
3. **Winner path:** acquire `ch->lock`, set `ch->closed = 1`, collect all fiber entries to wake, release the lock, spin+post each collected entry, call `chan_unregister`. **The mutex is left valid and intact** — ownership of `ch->lock` passes to the shutdown sweep in Step 3.

   `goc_close` uses a **collect-then-dispatch** protocol to avoid holding `ch->lock` during the spin on `fe->parked`. Under the lock it walks both `takers` and `putters`, calling `try_claim_wake` on each non-cancelled entry and collecting the `goc_entry*` (obtained via `mco_get_user_data(e->coro)`) for every `GOC_FIBER` entry into a heap-allocated `to_post` array. Non-fiber entries (`GOC_CALLBACK`, `GOC_SYNC`) are dispatched inline while the lock is still held, as their dispatch paths do not spin. After all entries are claimed, the lock is released, and the collected fiber entries are spun on and posted to the run queue one by one.

   The `to_post` array is heap-allocated (via `goc_malloc`) sized exactly to the number of non-cancelled waiters counted in a pre-pass. A fixed-size stack array must not be used — for large closes (e.g. 200k parked fibers in `bench_spawn_idle`) a cap silently drops wakeups, leaving fibers permanently suspended.

   When iterating takers and putters, `goc_close` must skip any entry where `e->cancelled == 1` **before** calling `try_claim_wake`. Cancelled entries belong to `goc_alts` losers whose coroutine is already `MCO_DEAD`; winning the CAS on such an entry and calling `post_to_run_queue` would call `mco_resume` on a dead coroutine, producing a SIGSEGV. This is the same guard applied by `wake()` and must be kept consistent with it.

   See [Unified Wakeup](#unified-wakeup) for the `e->next` snapshot requirement.

> **Why not destroy the mutex inline?** After `goc_close` wakes parked fibers, those fibers are re-enqueued on pool threads and will resume shortly. A resumed fiber may call `goc_take` or `goc_put` on the same channel pointer — the first thing those functions do is `uv_mutex_lock(ch->lock)`. If `goc_close` had already freed the mutex, that call would crash. The mutex must remain valid until the pool is fully drained and all workers are joined (Step 2). Only at that point is it guaranteed that no fiber can reach the channel's lock again.

### Parked Entry (`goc_entry`)

```c
typedef enum { GOC_FIBER, GOC_CALLBACK, GOC_SYNC } goc_entry_kind;

typedef struct goc_entry {
    goc_entry_kind    kind;
    uint64_t          id;           /* monotonically increasing fiber ID assigned at creation */
    void*             fiber_root_handle; /* opaque handle returned by goc_fiber_root_register */
    _Atomic int       cancelled;    /* alts: skip if set; written/read from multiple pool threads */
    _Atomic int       woken;        /* set by try_claim_wake before dispatch; prevents double-wake (second line of defence after fired) */
    _Atomic int*      fired;        /* shared flag for goc_alts; NULL for plain take/put entries; CAS'd 0→1 by try_claim_wake */
    struct goc_entry* next;
    goc_pool*         pool;         /* pool to re-enqueue this fiber on after a channel wake */
    struct goc_chan*   ch;           /* target channel for pending _cb operations */
    bool              is_put;       /* direction flag for pending _cb operations */

    /* GOC_FIBER */
    mco_coro*         coro;         /* minicoro coroutine handle */
    uint32_t*         stack_canary_ptr; /* points at the lowest word of the fiber stack;
                                           set at launch (goc_go) AND in the goc_take / goc_put
                                           slow paths; checked before every mco_resume —
                                           abort() on mismatch */
    void**            result_slot;  /* points at a void* local on the suspended fiber's stack */
    void            (*fn)(void*);   /* fiber entry function; on GC heap, safe if caller returns early */
    void*             fn_arg;
    goc_chan*         join_ch;      /* closed by the trampoline when the fiber returns;
                                       returned to the caller of goc_go / goc_go_on */

    /* all kinds */
    goc_status_t      ok;           /* GOC_OK = delivered successfully, GOC_CLOSED = channel closed */
    size_t            arm_idx;      /* index of this arm in ops[]; used by goc_alts */

    /* GOC_CALLBACK */
    void  (*cb)(void* val, goc_status_t ok, void* ud);
    void  (*put_cb)(goc_status_t ok, void* ud);  /* goc_put_cb completion; different sig from cb */
    void*  ud;
    void*  cb_result;               /* value storage; set result_slot = &e->cb_result for GOC_CALLBACK */
    void*  put_val;                 /* pending value for a parked GOC_CALLBACK putter */

    /* GOC_SYNC */
    goc_sync_t        sync_obj;     /* embedded portable condvar for single-entry sync callers */
    goc_sync_t*       sync_sem_ptr; /* pointer used by wake(); always &sync_obj for plain
                                       goc_take_sync / goc_put_sync; set to a shared stack
                                       goc_sync_t for goc_alts_sync arms so all arms post the
                                       same condvar without knowing whether it is embedded or shared */

    /* Yield-gate: guards the race between wake() and mco_yield().
     *
     * There is a brief window between when a parking fiber releases ch->lock
     * (or the alts lock set) and when it actually calls mco_yield. If wake()
     * calls post_to_run_queue during that window, a pool worker calls
     * mco_resume on a MCO_RUNNING coroutine — mco_resume silently returns
     * MCO_NOT_SUSPENDED, the entry is consumed, and the fiber hangs after
     * calling mco_yield with nobody left to resume it.
     *
     * Protocol (GOC_FIBER entries only):
     *   `parked` lives on the fiber's initial goc_entry (accessible via
     *   mco_get_user_data(e->coro)).  Values:
     *     0 = parking in progress (fiber set it just before releasing locks;
     *         mco_yield has not yet returned on the pool worker side)
     *     1 = fiber is safely MCO_SUSPENDED (pool_worker_fn set it after
     *         mco_resume returned)
     *
     *   Fiber (goc_take / goc_put / goc_alts slow path):
     *     Sets fiber_entry->parked = 0 while still holding the channel lock(s),
     *     then releases the lock(s) and calls mco_yield.
     *
     *   pool_worker_fn (after mco_resume returns):
     *     Sets fiber_entry->parked = 1.
     *
     *   wake(), wake_claim() callers, and goc_close() (GOC_FIBER case):
     *     Spin with sched_yield() while fiber_entry->parked == 0 before
     *     calling post_to_run_queue.  Guarantees the coroutine is truly
     *     MCO_SUSPENDED before any worker calls mco_resume.
     *     wake() spins while still holding ch->lock; wake_claim() callers
     *     and goc_close() release the lock first, then spin. */
    _Atomic int       parked;       /* per-fiber yield-gate; see protocol above */
} goc_entry;
```

---

## Fiber Mechanics

### Stack Management

Each fiber stack is created and managed by minicoro. Stack allocation, alignment, and guard-page setup are handled internally by minicoro — libgoc does not call `posix_memalign` or `mprotect` directly for fiber stacks.

Per-fiber stack size is controlled by the `LIBGOC_STACK_SIZE` CMake option (default `0`, passed to `mco_desc_init` as the `stack_size` argument). When `0`, minicoro uses its built-in default, which adapts to the active allocator. Avoid large stack allocations and deep recursion inside fiber entry functions — see [minicoro Limitations](#minicoro-limitations).

**Conditional stack overflow detection.** By default (canary mode), libgoc writes a sentinel canary word at the lowest address of each fiber stack immediately after `mco_create` returns. The worker loop checks the canary *before* every `mco_resume` call. If the canary has been overwritten, the runtime calls `abort()` with a diagnostic message identifying the corrupted fiber. This converts silent heap corruption into a deterministic, debuggable crash.

With virtual memory enabled (`-DLIBGOC_VMEM=ON`), stacks can grow dynamically and overflow detection is disabled for performance. The canary logic compiles to no-ops via conditional macros in `src/internal.h`. Stack protection is only meaningful with canary-protected stacks.

```c
/* Non-virtual memory mode: canary protection enabled */
#ifndef LIBGOC_VMEM_ENABLED
entry->stack_canary_ptr = (uint32_t*)entry->coro->stack_base;
*entry->stack_canary_ptr = GOC_STACK_CANARY;   /* e.g. 0xDEADC0DE */

if (*entry->stack_canary_ptr != GOC_STACK_CANARY)
    abort();   /* stack overflow detected — terminate immediately */
#endif
mco_resume(entry->coro);
```

```c
goc_chan* goc_go(void (*fn)(void*), void* arg);
goc_chan* goc_go_on(goc_pool*, void (*fn)(void*), void* arg);
```

`goc_go` posts to the current pool when called from a fiber, and falls back to the default pool outside fiber context. `goc_go_on` posts to a specific pool. Both return a join channel — see [Join Channel](#join-channel) below. Multiple pools allow priority separation or affinity pinning.

### Launch (`goc_go`)

```c
goc_chan* join_ch = goc_chan_make(0);   /* unbuffered; closed when fiber exits */
entry->join_ch = join_ch;

mco_desc desc = mco_desc_init(trampoline, stack_sz);
desc.user_data = entry;
mco_coro* coro;
mco_create(&coro, &desc);
entry->coro = coro;

/* write canary at the lowest address of the newly-created stack */
entry->stack_canary_ptr = (uint32_t*)coro->stack_base;
*entry->stack_canary_ptr = GOC_STACK_CANARY;

/* Submit the spawn to the pool. The pool may either materialise the fiber
    immediately or queue the spawn request behind the live-fiber admission cap,
    but the join channel is returned to the caller right away. */
pool_submit_spawn(pool, fn, arg, join_ch);
return join_ch;
```

### Join Channel

`goc_go` and `goc_go_on` both return a `goc_chan*` — the **join channel** — that is closed automatically when the fiber's entry function returns. The join channel is an ordinary unbuffered channel allocated by `goc_chan_make(0)` inside the launch path and stored in `entry->join_ch`. The fiber trampoline calls `goc_close(entry->join_ch)` as its very last action before the coroutine exits, waking any callers that are blocking on the channel.

Callers use the join channel to await fiber completion:

```c
/* from a plain OS thread */
goc_chan* done = goc_go(my_fn, arg);
goc_take_sync(done);   /* blocks until the fiber returns */

/* from another fiber */
goc_chan* done = goc_go(my_fn, arg);
goc_take(done);        /* suspends this fiber until the spawned fiber returns */

/* as one arm of a select, racing against a timeout */
goc_alt_op_t ops[] = {
    { .ch = done,              .op_kind = GOC_ALT_TAKE },
    { .ch = goc_timeout(5000), .op_kind = GOC_ALT_TAKE },
};
goc_alts_result_t* r = goc_alts(ops, 2);
```

The join channel may be ignored if no join is needed — the channel is GC-allocated and will be collected once it becomes unreachable after the fiber closes it.

### Suspend / Resume

Fibers suspend by calling `mco_yield` and are resumed by the pool worker calling `mco_resume`. Each fiber's stack must be scanned by the GC while the fiber is suspended — its local variables may hold pointers to GC-managed objects. Rather than calling `GC_add_roots`/`GC_remove_roots` around every yield (which would exhaust BDW-GC's fixed internal root-set table when large numbers of fibers are created), libgoc registers a `GC_push_other_roots` callback (see **GC Fiber Stack Roots** below) that scans all live fiber stacks at each GC mark phase.

Each fiber's stack is registered once at birth (in `goc_go_on`, `src/fiber.c`) and unregistered once at death (in `pool_worker_fn` before `mco_destroy`, `src/pool.c`).

---

## minicoro Limitations

libgoc uses [minicoro](https://github.com/edubart/minicoro) for all fiber switching. Four hard constraints apply to all fiber entry functions and must be understood by library embedders:

**C++ exceptions are not supported.** The C++ exception mechanism maintains internal unwinding state that is not preserved across a coroutine context switch. Throwing an exception that propagates across a `mco_yield` / `mco_resume` boundary is undefined behaviour and will typically corrupt the exception handler chain or crash. In mixed C/C++ codebases all fiber entry functions must be declared `extern "C"` and must not allow any C++ exception to escape them.

**Stack management.** By default, libgoc uses canary-protected stacks with overflow detection. This is the portable default and encourages library authors to develop with a platform-independent mindset. The virtual memory allocator can be enabled with `-DLIBGOC_VMEM=ON` for dynamic stack growth — useful when individual fibers need large or variable stacks. If stack overflow is detected in canary mode, the runtime calls `abort()` immediately with a diagnostic message. Avoid large stack-allocated buffers and deep recursion inside fibers regardless of stack mode — use `goc_malloc`-allocated buffers on the GC heap for large data.

**`src/minicoro.c` must be compiled without GC thread-wrapper defines (`-UGC_THREADS` and `-UGC_PTHREADS`).** minicoro declares a `static MCO_THREAD_LOCAL mco_current_co` variable (a `thread_local` TLS slot) to track the running coroutine per thread. When Boehm GC thread wrappers are active, thread-startup bookkeeping may walk TLS state during `pthread_create` startup. If minicoro's TLS block is visible before `mco_current_co` is initialised on a new thread, the GC can fault during startup before any minicoro API has been called. The fix is to compile `src/minicoro.c` in its own isolated translation unit with `-UGC_THREADS;-UGC_PTHREADS`, which `CMakeLists.txt` enforces via `set_source_files_properties`.

**`src/minicoro.c` must be compiled with `-DMCO_ZERO_MEMORY`.** minicoro's internal push/pop storage buffer is used to pass values between `mco_push`/`mco_pop` calls across a `mco_yield`/`mco_resume` boundary. When `MCO_ZERO_MEMORY` is not defined, popped bytes remain in the buffer after the pop — if those bytes held a GC-managed pointer, Boehm GC's conservative scanner may keep the referent alive indefinitely because it still sees a pointer-shaped value in the coroutine's allocation. Defining `MCO_ZERO_MEMORY` causes minicoro to `memset` the popped region to zero immediately after copying it to the caller, eliminating the stale-pointer window. This is minicoro's built-in GC-environment support and **must be enabled** when building with Boehm GC. `-UGC_THREADS;-UGC_PTHREADS` and `-DMCO_ZERO_MEMORY` are complementary: the former isolates minicoro from GC thread-wrapper startup paths; the latter ensures the conservative collector does not retain objects via stale references in coroutine storage.

**When `LIBGOC_VMEM=ON`, `src/minicoro.c` must also be compiled with `-DMCO_USE_VMEM_ALLOCATOR`.** This switches minicoro to its virtual-memory-backed stack allocator, matching libgoc's vmem mode (`LIBGOC_VMEM_ENABLED`) where canary checks are compiled out and stacks can grow dynamically.

---

## Channel Operation Flow

### `goc_take(ch)` — fiber context only

> **Must only be called from within a fiber.** The slow path calls `mco_running()` and asserts the result is non-NULL; calling `goc_take` from a bare OS thread aborts with a diagnostic message rather than silently crashing.

```
assert(mco_running() != NULL)   /* fiber-context guard */
uv_mutex_lock(ch)
if buffered value:
    dequeue → result
    wake oldest putter if any
    uv_mutex_unlock → return {result, GOC_OK}
if parked putter:
    take its value, wake it
    uv_mutex_unlock → return {value, GOC_OK}
if closed:
    uv_mutex_unlock → return {NULL, GOC_CLOSED}
else:
    allocate goc_entry (FIBER, GC heap)
    /* current_pool = ((goc_entry*)mco_get_user_data(mco_running()))->pool */
    entry.stack_canary_ptr = mco_running()->stack_base   /* required: pool worker checks this on re-resume */
    push onto ch->takers
    uv_mutex_unlock
    mco_yield → suspend

    /* resumes here */
    return {*result_slot, entry->ok}
```

`goc_put` is the exact mirror, with the same fiber-context assert and the same `stack_canary_ptr` initialization in its slow path. The `current_pool` retrieval is identical. Do not omit `stack_canary_ptr` from `goc_put` — the pool worker dereferences it on every resume (including re-resumes after a channel wake). When a parked fiber is woken, the runtime re-queues the coroutine's **initial** fiber entry (via `mco_get_user_data(e->coro)`), not the transient parked channel entry.

### `goc_take_cb(ch, cb, ud)` — any context

Non-blocking registration for non-fiber contexts (foreign threads, signal handlers, or code that cannot suspend). Parks a callback entry instead of suspending a fiber; `cb` is invoked on the uv loop thread when a value becomes available.

> **`cb` must not be `NULL`.** Unlike `goc_put_cb`, where `cb=NULL` is a valid fire-and-forget pattern (the caller just wants to deliver a value and move on), a take with no callback silently discards the received value — the entire point of a take is to receive. The implementation should assert that `cb != NULL`.

**Entry initialisation must happen before the lock.** The fast paths (value available, channel closed) allocate the entry, set `e->result_slot = &e->cb_result`, populate `e->cb` and `e->ud`, and then enqueue immediately without parking. Deferring initialisation to after the lock would leave `result_slot` unset when the fast path stores the dequeued value into it.

```
/* Allocate and fully initialise the entry before acquiring the lock so
   that both the fast path and the slow path can use it uniformly. */
allocate goc_entry (CALLBACK)
entry->cb         = cb
entry->ud         = ud
entry->result_slot = &entry->cb_result   /* storage for the received value */

uv_mutex_lock(ch)
if value available:
    dequeue value → entry->cb_result
    entry->ok = GOC_OK
    uv_mutex_unlock
    mpsc_push(&callback_queue, entry)
    uv_async_send(&wakeup_handle)   /* cb fires on loop thread via on_wakeup */
    return
if closed and empty:
    entry->ok = GOC_CLOSED
    uv_mutex_unlock
    mpsc_push(&callback_queue, entry)
    uv_async_send(&wakeup_handle)   /* cb fires with ok==GOC_CLOSED on loop thread */
    return
push entry onto ch->takers
uv_mutex_unlock
```

There is exactly one code path for delivering a value to a callback entry: enqueue to `callback_queue` and signal the loop via `uv_async_send`. `ud` is kept alive because `goc_entry` is GC-allocated and holds the pointer.

`goc_put_cb`: `cb` may be `NULL` when no completion notification is needed. In that case the value is delivered silently. If the channel is closed before delivery, the value is not consumed and no notification is issued. **When the value is `goc_malloc`-allocated (the recommended pattern), no explicit cleanup is required in the `ok==GOC_CLOSED` case — the GC reclaims it automatically. Only fall back to `malloc` and a non-NULL callback if the value must be explicitly freed on channel close.**

### `goc_take_sync(ch)` / `goc_put_sync(ch, val)`

Blocking calls for non-fiber threads (e.g. a plain `pthread` that needs to rendezvous with fiber code). Internally parks a `goc_sync_t` (portable mutex + condvar) rather than a fiber context; the calling OS thread blocks until a value is available. Calling from a fiber is a runtime invariant violation and aborts with a diagnostic message (use `goc_take`/`goc_put` there). Must not be called from a libuv callback — it would deadlock the event loop.

`goc_put_sync` flow:

```
uv_mutex_lock(ch)
if closed:
    uv_mutex_unlock → return GOC_CLOSED
if buffered space available:
    enqueue value
    wake oldest taker if any
    uv_mutex_unlock → return GOC_OK
if parked taker:
    deliver value directly, wake taker
    uv_mutex_unlock → return GOC_OK
else:
    allocate goc_entry (SYNC), init goc_sync_t
    push onto ch->putters
    uv_mutex_unlock
    goc_sync_wait(&entry->sync_obj)   /* block OS thread */

    /* resumes here */
    return entry->ok            /* GOC_OK = delivered, GOC_CLOSED = channel closed */
```

### `goc_take_try(ch)` — non-blocking, any context

Non-blocking attempt to receive from a channel. Returns immediately under lock with one of four outcomes: a buffered value (`GOC_OK`), a value from a parked putter (`GOC_OK`), closed-and-empty (`GOC_CLOSED`), or open-but-empty (`GOC_EMPTY`). Never parks.

`goc_take_try` does **not** trigger `compact_dead_entries`. It is a non-blocking fast path; compaction is left to the next `goc_take` or `goc_put` call on the same channel.

### Unified Wakeup

#### `wake` — claim and dispatch under lock

```c
bool wake(goc_chan* ch, goc_entry* e, void* value) {
    /* acquire ordering: see the full write that set `cancelled` before deciding to skip */
    if (atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
        ch->dead_count++;   /* entry stays on the list; compaction runs at next take/put */
        return false;
    }
    /* try_claim_wake performs two atomic CASes:
       1. If e->fired != NULL (a goc_alts entry), CAS fired 0→1 (acq_rel). If another
          arm already fired, return false — this is the core guard against double-wake
          when multiple channels close simultaneously while a fiber is parked in goc_alts.
       2. CAS woken 0→1 (acq_rel). Prevents a second wake on the same entry if two
          channels race before cancelled has propagated. */
    if (!try_claim_wake(e)) {
        ch->dead_count++;
        return false;
    }
    if (e->result_slot != NULL)
        *e->result_slot = value;
    e->ok = GOC_OK;
    if (e->kind == GOC_FIBER) {
        goc_entry* fe = (goc_entry*)mco_get_user_data(e->coro);
        while (atomic_load_explicit(&fe->parked, memory_order_acquire) == 0)
            sched_yield();
        post_to_run_queue(fe->pool, fe);
    } else if (e->kind == GOC_CALLBACK) {
        post_callback(e, value);   /* posted via uv_async_send; cb fires on loop thread */
    } else {
        /* GOC_SYNC: store result and unblock the waiting OS thread */
        goc_sync_post(e->sync_sem_ptr);
    }
    return true;
}
```

#### `wake_claim` — claim under lock, defer spin+post to caller

For call sites that must release `ch->lock` **before** spinning on `fe->parked` (to avoid holding the lock during a potentially long spin), `wake_claim` performs everything `wake` does except the spin and post for `GOC_FIBER` entries. Instead it returns the fiber's `goc_entry*` (`fe`) via an out-parameter, and the caller is responsible for the spin+post after unlocking:

```c
bool wake_claim(goc_chan* ch, goc_entry* e, void* value, goc_entry** fe_out);

// Caller contract for a non-NULL *fe_out:
//   1. Release ch->lock.
//   2. Spin: while (atomic_load(&fe->parked, acquire) == 0) sched_yield();
//   3. post_to_run_queue(fe->pool, fe);
```

`GOC_CALLBACK` and `GOC_SYNC` entries are dispatched inline inside `wake_claim` (those paths do not spin), and `*fe_out` is set to `NULL` for them. `wake_claim` is used by `goc_take`, `goc_put`, and `goc_close` so that the channel lock is never held during the yield-gate spin.

At the **top of `goc_take` and `goc_put`**, immediately after acquiring the lock, if `dead_count >= GOC_DEAD_COUNT_THRESHOLD`, walk both `takers` and `putters` and unlink every entry where `cancelled == 1`, then reset `dead_count` to 0. This amortises list cleanup without requiring inline removal at cancellation time. `compact_dead_entries` also repairs both tail pointers, so that append is O(1) again after a compaction sweep.

> **O(1) tail append via `chan_list_append`.** All slow-path append sites (`goc_take`, `goc_put`, `goc_alts`, `goc_take_cb`, `goc_put_cb`) use the `chan_list_append` helper (declared in `channel_internal.h`) instead of walking to the tail. The helper appends in O(1) when `takers_tail` / `putters_tail` is valid; it falls back to an O(N) repair walk only when the tail pointer has been invalidated by a prior removal. Removal operations in `chan_put_to_taker` and `chan_take_from_putter` invalidate the tail pointer when they remove the last remaining entry (detected by `next == NULL` after removal). Without this optimisation, `bench_spawn_idle` with N fibers all parking on a single channel performed O(N²) work during the spawn phase (N entries each requiring a full traversal), making 200k fibers take hundreds of seconds instead of tens.

> **`e->next` must be saved before calling `wake()` or `wake_claim()` in the splice helpers, and before any claim call in `goc_close`.** `chan_put_to_taker_claim` and `chan_take_from_putter_claim` both splice the woken entry out of its list with `*pp = e->next`. For `GOC_FIBER` entries, `wake()` (which spins+posts while holding the lock) can allow another worker to resume the fiber immediately. After dispatch, the transient parked entry may become unreachable and collectable, so reading `e->next` is unsafe. The fix is to snapshot `next = e->next` *before* calling `wake()` or `wake_claim()`, while `ch->lock` is still held (the lock prevents concurrent modification of the list), then use `next` for the splice. `goc_close` iterates with `while (e != NULL) { goc_entry* next = e->next; ..claim..; e = next; }`. This pattern must be preserved in any future modification of these helpers and of `goc_close`.

> **Why not `dead_count > buf_count + GOC_DEAD_COUNT_THRESHOLD`?** Scaling the threshold by `buf_count` would delay compaction dramatically on heavily-buffered channels — a channel with 1 024 buffered slots would accumulate 1 032 dead entries before a sweep. Comparing against a fixed threshold bounds the maximum number of stale entries regardless of buffer capacity.

---

## Thread Pool

```c
typedef struct {
    pthread_t        thread;
    goc_wsdq         deque;           /* Chase–Lev work-stealing deque (owner-push/pop + thief-steal) */
    goc_injector     injector;        /* MPSC queue: external callers push here; owner pops */
    uv_sem_t         idle_sem;        /* posted when work is available; worker sleeps on this */
    size_t           index;           /* worker index in pool->workers[] */
    goc_pool*        pool;            /* back-pointer to owning pool */
    _Atomic uint64_t steal_attempts;  /* total steal attempts from this worker's deque */
    _Atomic uint64_t steal_successes; /* successful steals from this worker's deque */
    uint32_t         miss_streak;     /* consecutive steal misses; resets on any successful dequeue or wakeup */
} goc_worker;

/* Pool-wide steal aggregates (file-scope globals in pool.c): */
/* static _Atomic uint64_t g_steal_attempts  = 0; */
/* static _Atomic uint64_t g_steal_successes = 0; */

typedef struct goc_pool {
    goc_worker*        workers;        /* array of thread_count workers */
    size_t             thread_count;
    size_t             max_live_fibers; /* 0 = unlimited; otherwise admission cap */
    _Atomic size_t     idle_count;     /* number of workers currently sleeping on idle_sem */
    _Atomic size_t     next_push_idx;  /* round-robin index for external injector pushes */
    _Atomic int        shutdown;       /* set to 1 to stop workers */

    /* Drain synchronisation — drain_mutex protects live_count,
       resident_count, pending_spawn_*, and drain_cond. */
    uv_mutex_t drain_mutex;
    uv_cond_t  drain_cond;
    size_t          live_count;        /* accepted spawn requests not yet completed, including
                                          deferred spawn requests waiting behind the admission cap */
    size_t          resident_count;    /* fibers that have an allocated coroutine/stack right now */
    goc_spawn_req*  pending_spawn_head;/* FIFO of deferred external spawns */
    goc_spawn_req*  pending_spawn_tail;
} goc_pool;
```

> **`drain_cond` is signalled only on `MCO_DEAD`.** The broadcast fires only when a fiber actually dies. `goc_pool_destroy` and `goc_pool_destroy_timeout` wait on `live_count == 0` — a parked fiber (MCO_SUSPENDED) is still alive and held in `live_count`, so the drain correctly waits until all fibers exit.

The default pool is created by `goc_init` with `max(4, hardware_concurrency)` worker threads. This can be overridden by setting the `GOC_POOL_THREADS` environment variable before calling `goc_init`.

**`LIBGOC_DEBUG`** — When defined at compile time (pass `-DLIBGOC_DEBUG=ON` to CMake), enables verbose `[GOC_DBG]` diagnostic output to `stderr` from the scheduler, I/O layer, and HTTP layer via the `GOC_DBG(fmt, ...)` macro defined in `include/goc.h`. When the flag is not set the macro expands to nothing, so the callsites produce zero code. Rebuild the library and tests with `-DLIBGOC_DEBUG=ON` to activate logging.

Each pool also has a **live-fiber admission cap**. By default it is
`floor(0.6 × (available_hardware_memory / fiber_stack_size))`
in both canary and vmem builds,
and it may be overridden with `GOC_MAX_LIVE_FIBERS` (`0` disables the cap).
The `0.6` factor intentionally keeps memory headroom for GC and runtime
overhead while still targeting high throughput. The cap limits
`resident_count` — the number of fibers that
have actually materialised a coroutine/stack — not `live_count`.

When an **external** caller (`main` thread, loop callback, or another pool)
spawns a fiber and the pool is already at its cap, `goc_go` / `goc_go_on`
still return a join channel immediately, but the actual fiber creation is
deferred by appending a small `goc_spawn_req` node to `pending_spawn_*`.
When any resident fiber reaches `MCO_DEAD`, the worker decrements
`resident_count`, then admits as many queued requests as the cap allows.

Same-pool spawns originating from a **currently running fiber** explicitly
*bypass* this cap. Without that bypass, parent→child dependency patterns such
as the benchmark prime sieve can deadlock: all resident slots become occupied
by parent fibers waiting for children that have been deferred behind the cap.

Each worker thread runs a 4-stage work-stealing loop:

```
while not shutdown:
    /* Stage 1: pop from own deque (LIFO, cache-warm) */
    entry = wsdq_pop_bottom(worker.deque)
    if entry != NULL: goto run

    /* Stage 2: drain own injector (entries posted by external callers) */
    entry = injector_pop(worker.injector)
    if entry != NULL: goto run

    /* Stage 3: steal phase — victim hinting, then randomised scan.
     * Skipped entirely when miss_streak >= STEAL_BACKOFF_THRESHOLD (default 8):
     * the worker falls through to Stage 4 and parks rather than spinning on
     * empty deques (IO-bound workloads where fibers are woken via uv_async). */
    if thread_count > 1 and worker.miss_streak < STEAL_BACKOFF_THRESHOLD:
        /* 3a. Victim hint: probe the last productive victim first */
        if worker.last_steal_victim != UNSET and != self:
            steal_attempts++
            entry = wsdq_steal_top(workers[last_steal_victim].deque)
            if entry != NULL:
                steal_successes++
                worker.miss_streak = 0
                goto run
            steal_misses++
            worker.miss_streak++
        /* 3b. Fallback: randomised scan over all other workers */
        xorshift32(seed)
        offset = 1 + (seed % (thread_count - 1))
        for i in 0..thread_count:
            victim = (self.index + offset + i) % thread_count
            if victim == self: continue
            steal_attempts++
            entry = wsdq_steal_top(workers[victim].deque)
            if entry != NULL:
                steal_successes++
                worker.last_steal_victim = victim
                worker.miss_streak = 0
                goto run
            steal_misses++
            worker.miss_streak++
        worker.last_steal_victim = UNSET   /* all failed; clear hint */

    /* Stage 4: go idle — sleep-miss race closure */
    /* 4a. Pre-park nudge: if our own deque still has work, wake a
     *     neighbour before parking so work is not left stranded. */
    if thread_count > 1:
        if wsdq_approx_size(worker.deque) > 0 and idle_count > 0:
            uv_sem_post(workers[(self+1) % thread_count].idle_sem)

    atomic_fetch_add(idle_count, 1, seq_cst)  /* must be seq_cst — pairs with post seq_cst fence */
    entry = wsdq_pop_bottom(worker.deque)      /* double-check: close race with concurrent post */
    if entry == NULL: entry = injector_pop(worker.injector)
    if entry != NULL:
        atomic_fetch_sub(idle_count, 1, relaxed)
        goto run
    uv_sem_wait(worker.idle_sem)               /* sleep until posted by post_to_run_queue */
    idle_wakeups++
    atomic_fetch_sub(idle_count, 1, relaxed)
    worker.miss_streak = 0  /* fresh steal budget after wakeup */
    continue

run:
    if stack canary protection is enabled and *entry->stack_canary_ptr != GOC_STACK_CANARY:
        abort()                        /* stack overflow — deterministic crash, not silent corruption
                                          applies to launch entries and park entries in canary mode;
                                          in vmem mode the check compiles away */
    mco_coro* coro = entry->coro       /* snapshot handle before resume — see note below */

     /* Redirect GC stack scan to fiber stack. */
     GC_get_my_stackbottom(&orig_sb)
    fiber_sb.mem_base = coro->stack_base + coro->stack_size
    GC_set_stackbottom(NULL, &fiber_sb)

    mco_resume(coro)                   /* run fiber until next mco_yield or return */

    GC_set_stackbottom(NULL, &orig_sb) /* restore OS thread stack bottom */

    fe = mco_get_user_data(coro)
    st = mco_status(coro)
    if st == MCO_SUSPENDED and fe != NULL:
        goc_fiber_root_update_sp(fe->fiber_root_handle, coro)  /* update cached SP for GC */
    /* Post-yield wakeup nudge: if our deque is more than half-full and an
     * idle worker exists, wake the next neighbour so it can steal. */
    if st == MCO_SUSPENDED and thread_count > 1:
        depth = wsdq_approx_size(worker.deque)
        if depth > worker.deque.capacity/2 and idle_count > 0:
            uv_sem_post(workers[(self+1) % thread_count].idle_sem)
    if fe != NULL: atomic_store(fe->parked, 1, release)  /* release yield-gate after resume bookkeeping */

    if st == MCO_DEAD:
        if fe != NULL: goc_fiber_root_unregister(fe->fiber_root_handle)  /* unregister via push_other_roots list */
        mco_destroy(coro)
        lock(drain_mutex)
            resident_count--
            live_count--
            admitted = dequeue pending_spawn_* while resident_count < max_live_fibers
            broadcast(drain_cond)
        unlock(drain_mutex)
        materialise and post all admitted spawn requests
    /* if MCO_SUSPENDED: the fiber yielded and is parked on a channel.
       live_count has NOT been decremented — the fiber is still alive.
       wake() → post_to_run_queue() will re-enqueue it without touching live_count.
       drain_cond is not broadcast here: live_count > 0 still holds, so
       goc_pool_destroy / goc_pool_destroy_timeout will not see a spurious drain-complete. */
```

> **Sleep-miss race closure.** A poster calling `post_to_run_queue` must not miss a sleeping worker. Protocol: (1) worker increments `idle_count` with `seq_cst` *before* its double-check; (2) poster completes its write (deque push or injector mutex-unlock, both full-barrier), then reads `idle_count` with `seq_cst`. The C11 total order on seq_cst operations guarantees that if the poster sees `idle_count == 0`, the worker's double-check will see the posted entry; if the poster sees `idle_count > 0`, it posts `idle_sem` to wake the worker.

> **Victim hinting.** Each worker maintains `last_steal_victim` (initialised to `SIZE_MAX` = unset). When a steal succeeds, the victim's index is stored there. On the next steal phase, that index is tried first (before the randomised scan), because a recently productive victim is likely still busy. If the hint steal fails or the index is unset, the worker falls back to the randomised xorshift scan. The hint is cleared when all steal attempts in a round fail. This reduces the average number of probes needed to find work in sustained high-throughput workloads.

> **IO-aware steal backoff (`miss_streak`).** Each worker maintains a `uint32_t miss_streak` counter that increments on every `wsdq_steal_top` miss (both hint-path and randomised-scan) and resets to 0 on any successful dequeue (own-deque pop, injector drain, or steal hit) and after every `uv_sem_wait` wakeup. When `miss_streak >= STEAL_BACKOFF_THRESHOLD` (default: 8, defined in `src/config.h`), the worker skips Stage 3 entirely and parks immediately at Stage 4. This suppresses hot spinning on IO-bound workloads (e.g. HTTP) where runnable fibers are woken via `uv_async` and never appear on a deque at probe time — the observed 90–95% steal miss rates in HTTP benchmarks are collapsed to zero wasted probes per wakeup cycle. CPU-bound workloads (steal frequently succeeds) never accumulate a streak and are unaffected.

> **Steal and idle counters.** `steal_attempts`, `steal_successes`, and `steal_misses` are per-worker relaxed atomics accumulated on every `wsdq_steal_top` call (both hint-path and randomised-scan). `idle_wakeups` is incremented once per `uv_sem_wait` return. All four are mirrored to global aggregates (`g_steal_*`, `g_idle_wakeups` in `pool.c`) and exposed via `goc_pool_get_steal_stats`. Per-worker totals are also reported in the `GOC_WORKER_STOPPED` telemetry event (attempts and successes only). A high `misses`-to-`attempts` ratio indicates contention on steal targets; a high `idle_wakeups`-to-`successes` ratio indicates steal thrashing or spurious wakeups.


> **Why `coro` is snapshotted before `mco_resume`.** `post_to_run_queue` may receive either a fiber's initial entry (launch/resume token) or another scheduler-visible entry that refers to the same coroutine. Snapshotting `coro = entry->coro` before `mco_resume` ensures the worker keeps a stable handle even if other state associated with the scheduling token becomes irrelevant once the coroutine advances. The `mco_coro` object itself remains valid until `mco_destroy`.

> **GC Stack Bottom Redirect.** When `mco_resume` switches the CPU stack pointer (RSP) from the worker OS thread's stack into the fiber's stack, the Boehm GC stop-the-world handler captures that fiber-stack RSP as the "bottom of the stack to scan". GC then tries to scan from that RSP all the way up to the worker thread's registered OS-stack bottom — a range potentially spanning the entire virtual address space between the two stacks. That range contains unmapped pages, causing SIGSEGV inside the GC marker thread on every collection cycle.
>
> **Fix:** Before each `mco_resume`, the worker captures its current OS-thread stack bottom in `orig_sb`, then redirects GC scanning to `[fiber_RSP, fiber_stack_top]` with `GC_set_stackbottom(NULL, &fiber_sb)`. After `mco_resume` returns (fiber has yielded or exited), `GC_set_stackbottom(NULL, &orig_sb)` restores the worker's OS-thread stack bottom. This keeps the GC scan range valid for the entire duration of fiber execution and eliminates the inter-stack SIGSEGV.

> **GC Fiber Stack Roots.** While a fiber is suspended between `mco_yield` and the next `mco_resume`, its stack is not associated with any OS-thread stack that the GC scans automatically. The fiber's local variables may hold pointers to GC-managed objects, so the fiber stack must be treated as a GC root during the suspension window.
>
> **Why not `GC_add_roots`?** BDW-GC's `GC_add_roots` / `GC_remove_roots` store entries in a fixed internal table (`MAX_ROOT_SETS` ≈ 2048). Benchmarks that spawn large numbers of fibers (e.g. `bench_spawn_idle` with 200 000 fibers) exhaust this table, causing BDW-GC to abort with `"Too many root sets"`.
>
> **Fix:** `goc_init` registers a `GC_push_other_roots` callback (`push_fiber_roots` in `src/gc.c`) that scans all live fiber stacks and calls `GC_push_all_eager` for each during every GC mark phase.  Live fibers are tracked with a **flat array + atomic bitmap** (Fix 5): the slot array is divided into CHUNK_SIZE-slot chunks allocated on demand; a parallel `_Atomic(uint64_t)` bitmap encodes liveness (1 = slot in use).  The `push_fiber_roots` callback iterates bitmap words — skipping dead 64-slot regions in a single comparison — then visits only the set bits.  This avoids pointer-chasing through dead linked-list nodes and immediately reclaims slots of exited fibers for reuse.
>
> **Thread-safety.** Registration and unregistration use `fiber_root_mutex` (a plain `uv_mutex_t`).  Growth (adding a new chunk) uses `calloc` — not `GC_malloc` — so the GC cannot be triggered while the mutex is held.  The `push_fiber_roots` callback reads the chunk pointer array and bitmap WITHOUT holding `fiber_root_mutex` (a mutex in the callback would deadlock if a stopped thread held it).  Growth publishes the new chunk pointer to `fiber_root_chunks[c]` before atomically incrementing `fiber_root_num_chunks` with a `memory_order_release` fence; the callback's `memory_order_acquire` load of `num_chunks` guarantees it sees a consistent snapshot.  Unregistration clears the bitmap bit with `memory_order_release` via `atomic_fetch_and`.
>
> **Handle.** `goc_fiber_root_register` returns the slot index cast to `void*`.  This keeps the handle valid across potential future growth (which appends new chunks without moving old ones).
>
> **Slot fields.** Each `fiber_root_slot` stores four fields:
> - `stack_base` (`void*`) — low end of the fiber stack (constant after init); the GC scan always starts here.
> - `stack_top` (`void*`) — high end of the fiber stack (constant after init).
> - `scan_from` (`_Atomic(void*)`) — cached saved RSP/SP of the suspended fiber, initialised by `mco_get_suspended_sp` at birth and updated by `goc_fiber_root_update_sp` after each `mco_resume` returns `MCO_SUSPENDED`; kept for potential future optimisation but **not currently used** for scanning — `push_fiber_roots` always scans `[stack_base, stack_top]` to avoid missing pointers in active call frames that lie below a stale saved SP.
> - `entry` (`goc_entry*`) — the fiber's GC-allocated entry; explicitly pushed via `GC_push_all_eager(&s->entry, …)` to keep it alive (the run queue node referencing it is `malloc`'d, not GC-managed).
>
> **Shutdown cleanup.** `goc_shutdown()` frees all chunk arrays, zeros the used bitmap words, resets `fiber_root_num_chunks` to 0, and destroys `fiber_root_mutex`.

The invariant is:

- `pool_submit_spawn` increments `live_count` exactly once per accepted spawn request, whether the fiber is materialised immediately or queued in `pending_spawn_*`.
- `resident_count` counts only fibers that already have a coroutine/stack. Deferred spawn requests contribute to `live_count` but not to `resident_count`.
- `post_to_run_queue` pushes the entry to the run queue. It does **not** touch `live_count` — re-queuing a parked fiber must not inflate it.
- `live_count` and `resident_count` are decremented **only** when `mco_status(coro) == MCO_DEAD`. At that same point, the worker may admit deferred external spawns until the cap is full again.
- `goc_pool_destroy` and `goc_pool_destroy_timeout` wait on `live_count == 0`. This correctly waits for both already-materialised fibers and deferred-but-accepted spawns to finish.

`goc_pool_destroy` is a **blocking drain-and-join**. It waits in a loop on `drain_cond` (under `drain_mutex`), re-checking `live_count > 0` on each wake to guard against spurious wakeups, until `live_count` reaches zero, meaning every fiber launched on this pool has actually returned. Only then does it signal `shutdown = 1`, post the semaphore to unblock sleeping workers, and join all threads. It is safe to call `goc_pool_destroy` while fibers are still queued, running, or parked — the call is itself the synchronisation barrier.

`goc_pool_destroy` must **not** be called from within one of the target pool's own worker threads (including from a fiber running on that pool). That self-destroy path would attempt to join the calling thread and deadlock; the implementation detects this and calls `abort()` with a diagnostic message.

`goc_pool_destroy_timeout(pool, ms)` is the non-blocking variant. It waits on `drain_cond` with a deadline of `ms` milliseconds. If `live_count` reaches zero before the deadline it performs the same shutdown-and-join sequence as `goc_pool_destroy` and returns `GOC_DRAIN_OK`. If the deadline expires while `live_count > 0` it returns `GOC_DRAIN_TIMEOUT` immediately — **the pool is not destroyed** and remains fully valid. Worker threads continue executing. The caller may retry `goc_pool_destroy_timeout`, call `goc_pool_destroy` for an unconditional wait, or close the channels that parked fibers are blocked on to unblock them before retrying.

`goc_pool_destroy_timeout` has the same self-call restriction: invoking it from a worker thread that belongs to `pool` is invalid and aborts with a diagnostic message.

> **Scope:** `live_count` only tracks fibers on *this specific pool*. Fibers on other pools (including the default pool) are not counted. `goc_pool_destroy` will block indefinitely if any fiber **on this pool** is parked on a channel event that will never arrive — for example, if a fiber launched via `goc_go_on(this_pool, ...)` is waiting on a channel that nothing will ever write to.



Each pool uses per-worker **Chase–Lev work-stealing deques** (`goc_wsdq`) and per-worker **MPSC injector queues** (`goc_injector`) instead of a single shared run queue.

```c
/* goc_wsdq — Chase–Lev circular work-stealing deque */
typedef struct {
    _Atomic size_t       bottom;     /* owner's push/pop cursor (write end) */
    _Atomic size_t       top;        /* thieves' steal cursor (read end) */
    _Atomic(goc_entry**) buf;        /* circular buffer, capacity always a power of two */
    size_t               capacity;
    uv_mutex_t           steal_lock; /* serialises concurrent thieves; not held by owner */
} goc_wsdq;

/* goc_injector — MPSC queue (mutex-protected singly-linked list) */
typedef struct {
    goc_injector_node* head;   /* pop end (owner only) */
    goc_injector_node* tail;   /* push end (any thread) */
    uv_mutex_t         lock;
} goc_injector;
```

`wsdq_push_bottom` / `wsdq_pop_bottom` are called only by the owning worker (no lock on the fast path). `wsdq_steal_top` is called by any thread under `steal_lock`. `wsdq_pop_bottom` uses `atomic_fetch_sub(seq_cst)` to decrement `bottom` and checks emptiness using `old_b <= top` (pre-decrement value) to avoid unsigned wraparound when `bottom == 0`.

`post_to_run_queue` routes entries as follows:

- **Internal caller** (a fiber running on a pool thread, i.e. `tl_worker != NULL && tl_worker->pool == pool`): pushes to the executing worker's own deque and does **not** proactively wake peers. This preserves locality for short handoff-heavy workloads.
- **External caller** (main thread, libuv loop, another pool): pushes into a round-robin target worker's injector, then posts that worker's `idle_sem` if any worker is idle.

All threads — pool workers and the uv loop thread — are created via `goc_thread_create` (implemented in `gc.c`). It wraps `uv_thread_create` with a trampoline (`goc_thread_trampoline`) that calls `GC_register_my_thread` at thread start and `GC_unregister_my_thread` at thread exit on all platforms.

> **`_Thread_local` and minicoro.** `tl_worker` is a `_Thread_local` pointer to the currently executing `goc_worker`. minicoro switches stacks, not OS threads, so `tl_worker` always reflects the OS thread running the fiber and is always correct for internal/external routing decisions. A stolen fiber running on a different worker correctly sees `tl_worker == &workers[thief_index]` after the steal.

---

## Cross-Thread Wakeup via `uv_async_t`

When a pool thread wakes a callback entry, it needs to post work back safely. A shared `uv_async_t` (malloc-allocated) handles this:

```c
/* init */
uv_async_init(loop, &wakeup_handle, on_wakeup);

/* pool thread: enqueue callback entry, signal loop only on first enqueue */
size_t prev_depth = mpsc_push(&callback_queue, entry);
if (prev_depth == 0)
    uv_async_send(&wakeup_handle);  /* coalesced: skip if queue already non-empty */

/* loop thread: drain queue, invoke callbacks */
static void on_wakeup(uv_async_t* h) {
    goc_entry* e;
    while ((e = mpsc_pop(&callback_queue))) {
        if (e->cb)      /* take callback: receives the value, status, and user data */
            e->cb(e->cb_result, e->ok, e->ud);
        else if (e->put_cb)  /* put callback: receives only status and user data */
            e->put_cb(e->ok, e->ud);
    }
}
```

`uv_async_send` is the only libuv call made from pool threads. It is explicitly safe to call from any thread.

> **`post_callback` coalesces wakeup signals: `uv_async_send` is called only when the queue transitions from empty to non-empty (pre-push depth == 0).** When depth > 0 a prior send is already in flight and libuv will fire `on_wakeup` to drain everything in the queue at that point, including the newly enqueued entry. This eliminates the per-enqueue `uv_async_send` cost within a burst, directly reducing idle wakeup count.
>
> **`post_callback` also guards against calling `uv_async_send` on a closed handle using an atomic acquire/release pair.** `g_wakeup` is declared `_Atomic(uv_async_t *)`. `goc_init` stores the handle pointer with `memory_order_release` after `uv_async_init` completes. `on_wakeup_closed` stores `NULL` with `memory_order_release` once the handle is fully closed. `post_callback` reads with `memory_order_acquire`, guaranteeing it either sees a fully-initialised handle or NULL — never a partial or stale pointer. A pool worker woken by `goc_shutdown`'s Step 1 `goc_close` loop may call `post_callback` after the handle is closed; the acquire load returns NULL and the send is skipped. The entry is already safely enqueued; `on_shutdown_signal` drains the queue before the loop exits:
>
> ```c
> void post_callback(goc_entry* e, void* value) {
>     /* ... push to g_cb_queue, returns pre-push depth ... */
>     size_t prev_depth = mpsc_push(&g_cb_queue, node);
>     uv_async_t* wakeup = atomic_load_explicit(&g_wakeup, memory_order_acquire);
>     if (wakeup && prev_depth == 0)
>         uv_async_send(wakeup);  /* first enqueue in this burst — wake loop thread */
> }
> ```

> **`g_wakeup` is set to `NULL` inside `on_wakeup_closed`, not in `goc_shutdown` itself.** After the shutdown signal triggers `uv_close` on `g_wakeup`, libuv may still have queued internal events that reference the handle. Zeroing `g_wakeup` before the close callback fires would create a window in which the loop thread could dereference a null pointer. Instead the pointer is cleared only inside the `uv_close` completion callback, at which point libuv guarantees no further callbacks will fire:
>
> ```c
> static void on_wakeup_closed(uv_handle_t* h) {
>     free(h);
>     atomic_store_explicit(&g_wakeup, NULL, memory_order_release);
> }
> ```

> **`uv_close` on `g_wakeup` must always be called from the loop thread, never from `goc_shutdown` (the main thread).** `uv_close` is not documented as thread-safe; calling it from a non-loop thread races against the loop thread's handle-list traversal. The correct pattern — implemented via `g_shutdown_async` and `on_shutdown_signal` — is to send a signal that the loop thread acts on, so that both `uv_close` calls originate on the loop thread itself.

---

## MPSC Callback Queue

The MPSC queue (`g_cb_queue`) uses a Vyukov-style intrusive design. Nodes are **GC-allocated via `goc_malloc`** so that the `goc_entry*` payload each node carries remains visible to the collector between `mpsc_push` and `mpsc_pop`. Once `q->tail` advances past a node, that node is no longer reachable from the queue and the GC reclaims it implicitly — no explicit `free` is needed or performed on popped nodes.

### Initialization

`g_cb_queue` is a file-scope global in `loop.c`. It must be explicitly initialized before any call to `post_callback` or `on_wakeup`. This is done by `cb_queue_init()`, which is called from `goc_init` immediately after the GC is ready (so `goc_malloc` works) and before the loop thread is spawned.

```c
void cb_queue_init(void) {
    mpsc_init(&g_cb_queue);   /* allocates a sentinel stub node via goc_malloc */
}
```

`cb_queue_init` allocates a fresh sentinel node via `goc_malloc` and must be called from `goc_init` before the loop thread is spawned, so that `post_callback` can safely call `mpsc_push` from the moment `goc_init` returns.



---

## `goc_timeout` — libuv Timer

`goc_timeout_req` and `goc_timeout_timer_ctx` are GC-allocated and registered via `gc_handle_register` — see [libuv Role](#libuv-role). `goc_timeout_timer_ctx` is unregistered in its `uv_close` callback via `gc_handle_unregister`, after which the GC may collect it.

`uv_timer_init` and `uv_timer_start` are **not thread-safe**; they must be called from the loop thread. Because `goc_timeout` may be called from any thread (fiber, pool worker, main thread), the timer initialisation is marshalled onto the loop thread via `post_on_loop()`:

```c
typedef struct {
    struct goc_timeout_timer_ctx* timer_ctx;
    uint64_t    ms;
    uint64_t    deadline_ns; /* uv_hrtime() snapshot taken at goc_timeout() call time */
} goc_timeout_req;

typedef struct goc_timeout_timer_ctx {
    uv_timer_t timer;        /* MUST be first */
    goc_chan*  ch;
} goc_timeout_timer_ctx;

goc_chan* goc_timeout(uint64_t ms) {
    goc_chan* ch = goc_chan_make(0);
    goc_timeout_req*       req  = goc_malloc(sizeof(goc_timeout_req));
    goc_timeout_timer_ctx* tctx = goc_malloc(sizeof(goc_timeout_timer_ctx));
    tctx->ch = ch;
    req->timer_ctx = tctx; req->ms = ms;
    req->deadline_ns = uv_hrtime();
    gc_handle_register(tctx);  /* pin while libuv holds &tctx->timer */
    post_on_loop(on_start_timer, req);
    return ch;
}

/* runs on the loop thread */
static void on_start_timer(void* arg) {
    goc_timeout_req* req = (goc_timeout_req*)arg;
    /* Subtract async dispatch latency so the timer fires at the wall-clock
     * deadline recorded above, not req->ms after this callback happens to run.
     * Clamp to zero if the deadline has already passed — fire next iteration. */
    uint64_t now_ns     = uv_hrtime();
    uint64_t elapsed_ms = (now_ns > req->deadline_ns)
                        ? (now_ns - req->deadline_ns) / 1000000ULL : 0;
    uint64_t remaining  = (elapsed_ms < req->ms) ? (req->ms - elapsed_ms) : 0;
    uv_timer_init(g_loop, &req->timer_ctx->timer);
    uv_timer_start(&req->timer_ctx->timer, on_timeout, remaining, 0);
}

static void on_timeout(uv_timer_t* t) {
    goc_timeout_timer_ctx* tctx = (goc_timeout_timer_ctx*)t;
    goc_close(tctx->ch);        /* wake any parked takers with ok==GOC_CLOSED */
    uv_close((uv_handle_t*)t, unregister_handle_cb);
    /* goc_close is safe to call concurrently with goc_shutdown's Step 1 close
       loop because it is serialised by a close_guard CAS: exactly one caller
       flips close_guard from 0→1 and proceeds with teardown; the other sees 1
       and returns immediately — no double-free, no segfault. */
}

static void unregister_handle_cb(uv_handle_t* h) {
    gc_handle_unregister(h);   /* remove from live_uv_handles; GC may now collect */
}
```

> **Dispatch-latency correction.** `uv_timer_start` is called from `on_start_timer`, which runs on the loop thread some time *after* `goc_timeout()` returned to the caller. Without correction, the observable latency from `goc_timeout()` to channel-close would be `async_dispatch_delay + ms`, not `ms`. On a loaded system the dispatch delay can be hundreds of milliseconds — large enough to exceed any reasonable upper-bound assertion in tests. The fix is to snapshot `uv_hrtime()` at call time, compute how many milliseconds of the budget were consumed by the dispatch, and pass the reduced `remaining` duration to `uv_timer_start`. If the budget is already exhausted (`remaining == 0`), libuv fires the timer on the very next loop iteration, which is the correct expired-deadline behaviour.

Runs entirely on the uv loop thread (after the async dispatch). Composes with `goc_alts` for deadline semantics:

```c
goc_alt_op_t ops[] = {
    { .ch = data_ch,          .op_kind = GOC_ALT_TAKE },
    { .ch = goc_timeout(500), .op_kind = GOC_ALT_TAKE },
};
goc_alts_result_t* r = goc_alts(ops, 2);
```

---

## `goc_alts`

```c
typedef enum {
    GOC_ALT_TAKE,    /* receive from ch */
    GOC_ALT_PUT,     /* send put_val into ch */
    GOC_ALT_DEFAULT, /* fires immediately if no other arm is ready; ch must be NULL */
} goc_alt_kind_t;

typedef struct { goc_chan* ch; goc_alt_kind_t op_kind; void* put_val; } goc_alt_op_t;
typedef struct { goc_chan* ch; goc_val_t value; } goc_alts_result_t;

goc_alts_result_t* goc_alts     (goc_alt_op_t* ops, size_t n); /* fiber context */
goc_alts_result_t* goc_alts_sync(goc_alt_op_t* ops, size_t n); /* blocking OS thread */
```

`goc_alts` may suspend the calling fiber and must only be called from within a fiber. `goc_alts_sync` blocks the calling OS thread; calling it from within a fiber is a runtime invariant violation and aborts with a diagnostic message.

The returned `goc_alts_result_t->ch` is the channel pointer of the winning arm; it is `NULL` when the `GOC_ALT_DEFAULT` arm fires. `goc_alts_result_t->value` is a `goc_val_t`. For take arms, `value.ok == GOC_CLOSED` means the channel was closed rather than that a `NULL` was sent. For put arms, the winning result is always `{NULL, GOC_OK}` — `result_slot` is NULL for put entries and `wake()` skips the `result_slot` write. For a `GOC_ALT_DEFAULT` arm, the winning result is `{NULL, GOC_OK}`.

**Invariants:**

- **At most one `GOC_ALT_DEFAULT` arm** must be provided. If `ops` contains more than one arm with `op_kind == GOC_ALT_DEFAULT`, both functions abort with a diagnostic error message. Setting `op_kind` to `GOC_ALT_DEFAULT` requires `ch == NULL`; providing a non-NULL channel pointer with `GOC_ALT_DEFAULT` is unsupported and produces undefined behaviour.
- **Fiber context (goc_alts only):** `goc_alts` must be called from within a fiber (i.e., `mco_running() != NULL`). Calling `goc_alts` from a bare OS thread aborts with a diagnostic error message.

**Phases:**

1. Shuffle `ops` to avoid starvation. A `GOC_ALT_DEFAULT` arm, if present, is excluded from the shuffle and always treated last in Phase 2.
2. Try each non-default arm non-blocking using `op_kind` to determine direction (`GOC_ALT_TAKE` or `GOC_ALT_PUT`) — first success returns immediately. If no arm fires and a `GOC_ALT_DEFAULT` arm is present, return immediately with that arm's index and `{NULL, GOC_OK}` — the fiber is never suspended.

   For **take arms**: if the channel is closed and empty, return `{NULL, GOC_CLOSED}` with the arm's index immediately. For **put arms**: if the channel is closed, likewise return `{NULL, GOC_CLOSED}` with the arm's index immediately. Both directions are symmetrical — a closed channel is a terminal condition and must not fall through to the DEFAULT arm or the park phases.
3. Allocate one `goc_entry` per **non-default** op **and** a single `_Atomic int fired` flag (all via `goc_malloc`). Every entry's `fired` pointer points at this shared flag. The `fired` object lives on the GC heap alongside the entries for the duration of the select. `GOC_ALT_DEFAULT` arms do not produce entries because they are only reachable in Phase 2.

   **`stack_canary_ptr` must be set for every `GOC_FIBER` park entry.** `pool_worker_fn` unconditionally dereferences `entry->stack_canary_ptr` before calling `mco_resume` on any entry it pops from the run queue. Because park entries are posted to the run queue by `wake()` when the winning channel fires, they pass through this check just like launch entries. Set `e->stack_canary_ptr = (uint32_t *)running->stack_base` — the same value written by fiber launch — so the canary check reads the word at the bottom of the fiber's stack and detects overflow correctly. Leaving `stack_canary_ptr` as `NULL` causes a null-pointer dereference the moment the first park path is exercised.

   **Lock setup:** Build a deduplicated, sorted copy of the channel pointers to establish acquisition order. Stack-allocate this scratch buffer for small `n` (≤ `GOC_ALTS_STACK_THRESHOLD`), or `malloc`/`free` it immediately after the locks are acquired. Do **not** allocate it via `goc_malloc`. The buffer's lifetime is strictly bounded to this call frame: it is freed (or goes out of scope) before the fiber parks, so there is no suspension window during which the GC could observe or need to trace it. Plain `malloc` is therefore correct and avoids polluting the GC heap with short-lived, pointer-free scratch memory.

4. **Acquire channel locks in ascending pointer-address order** to prevent deadlock when multiple channels are locked simultaneously.
5. **Re-attempt under lock:** If any arm succeeds before entries are appended to any channel list, release the locks and return immediately. Do **not** write `cancelled` on the other entries — they have not been enqueued and `wake()` will never see them.

   **Park:** For `goc_alts`: call `mco_yield` to suspend the fiber. For `goc_alts_sync`: block on a shared `goc_sync_t` (portable mutex + condvar). Copy the fiber/condvar context into every `entries[i]`. Set `entries[i]->arm_idx = i`.

   **`goc_alts_sync` teardown:** After `goc_sync_wait` returns, call `goc_sync_destroy(&shared_sync)` before returning. This is safe: `wake` wins both CASes inside `try_claim_wake` and completes `goc_sync_post` before any other path can call `goc_sync_post` on the same object. All other entries either have their `cancelled` flag set or lose one of the CASes in `try_claim_wake`, so no additional `goc_sync_post` on `shared_sync` will occur after `goc_sync_wait` unblocks. `goc_sync_destroy` may therefore be called immediately.

6. **On wake (park path only):** The first channel to call `try_claim_wake` wins by performing a CAS on `fired` (0→1, `acq_rel`) followed by a CAS on `woken` (0→1, `acq_rel`). `wake()` then writes the delivered value into `result_slot` and calls `post_to_run_queue`. The write to `result_slot` happens before the entry is enqueued for the resuming pool thread, so the fiber observes the correct value when it reads `result_slot` after `mco_yield` returns. The resuming fiber then iterates all entries and writes `cancelled = 1` on every entry where `woken == 0` (the losers).

   > **Fast-path return does not write `cancelled`.** When Phase 5's re-attempt under lock succeeds before any entries are appended to a channel list, `goc_alts` returns immediately. At that point the entries have never been enqueued, so no channel can ever call `wake()` on them — writing `cancelled` would be harmless but is unnecessary and must not be done unconditionally. Only the park path (where entries were appended to channel lists and the fiber yielded) must iterate and cancel the losing entries on resume. The allocated `goc_entry` objects and the `fired` flag are simply abandoned — all are GC-heap allocated and the collector reclaims them automatically. No explicit free or cancel is required.

**Winner/loser entry states after wake:**

| `woken` | `ok` | meaning |
|---|---|---|
| 1 | `GOC_OK` | winner — value delivered |
| 1 | `GOC_CLOSED` | winner — channel was closed |
| 0 | `GOC_CLOSED` | loser — `cancelled=1` written; entry skipped by `wake()` |

`cancelled=1` is written only on entries where `woken == 0`. The winner is the unique entry where `woken == 1`.

No queue removal. Stale entries are skipped on dequeue.

---

## Public API

```c
/* Types */
typedef enum {
    GOC_EMPTY  = -1,  /* channel open but no value available (goc_take_try only) */
    GOC_CLOSED =  0,  /* channel was closed */
    GOC_OK     =  1,  /* value delivered successfully */
} goc_status_t;
typedef struct { void* val; goc_status_t ok;          } goc_val_t;
typedef enum   { GOC_ALT_TAKE, GOC_ALT_PUT,
                 GOC_ALT_DEFAULT                       } goc_alt_kind_t;
typedef struct { goc_chan* ch; goc_alt_kind_t op_kind;
                 void* put_val;                        } goc_alt_op_t;
typedef struct { goc_chan* ch; goc_val_t value;        } goc_alts_result_t;
typedef enum {
    GOC_DRAIN_OK      = 0,  /* all fibers finished within the deadline */
    GOC_DRAIN_TIMEOUT = 1,  /* deadline expired; pool remains valid and running */
} goc_drain_result_t;

/* Init / shutdown */
void          goc_init(void);
void          goc_shutdown(void);

/* Lifecycle invariant: both functions must be called from the process
 * main thread. Non-main-thread calls print an error to stderr and abort(). */

/* Memory */
void*         goc_malloc(size_t n);
void*         goc_realloc(void* ptr, size_t n);
char*         goc_sprintf(const char* fmt, ...);

/* Scalar boxing helpers (macros) */
/* goc_box_int(x)    — (void*)(intptr_t)(x)  */
/* goc_unbox_int(p)  — (intptr_t)(p)          */
/* goc_box_uint(x)   — (void*)(uintptr_t)(x) */
/* goc_unbox_uint(p) — (uintptr_t)(p)         */

/* Channels */
goc_chan*     goc_chan_make(size_t buf_size);
void          goc_close(goc_chan* ch);

/* Utilities */
bool          goc_in_fiber(void);   /* true if caller is inside a fiber, false otherwise */

/* Fiber launch — both return a join channel closed when the fiber returns */
goc_chan*     goc_go(void (*fn)(void*), void* arg);
goc_chan*     goc_go_on(goc_pool* pool, void (*fn)(void*), void* arg);

/* Channel I/O — fiber context */
goc_val_t*    goc_take(goc_chan* ch);
goc_status_t  goc_put(goc_chan* ch, void* val);

/* Channel I/O — non-blocking (any context) */
goc_val_t*    goc_take_try(goc_chan* ch);   /* ok==GOC_EMPTY if open but empty */

/* Channel I/O — blocking OS thread */
goc_val_t*    goc_take_sync(goc_chan* ch);
goc_status_t  goc_put_sync(goc_chan* ch, void* val);

/* Channel I/O — callbacks (any context; cb invoked on uv loop thread) */
void          goc_take_cb(goc_chan* ch,
                          void (*cb)(void* val, goc_status_t ok, void* ud), void* ud);
void          goc_put_cb(goc_chan* ch, void* val,
                         void (*cb)(goc_status_t ok, void* ud), void* ud);

/* RW mutexes (channel-backed lock handles) */
goc_mutex*    goc_mutex_make(void);
goc_chan*     goc_read_lock(goc_mutex* mx);
goc_chan*     goc_write_lock(goc_mutex* mx);

/* Select */
goc_alts_result_t* goc_alts     (goc_alt_op_t* ops, size_t n);
goc_alts_result_t* goc_alts_sync(goc_alt_op_t* ops, size_t n);

/* Timeout */
goc_chan*     goc_timeout(uint64_t ms);

/* Thread pool */
goc_pool*          goc_pool_make(size_t threads);
goc_pool*          goc_default_pool(void);
void               goc_pool_destroy(goc_pool* pool);         /* blocking drain-and-join */
goc_drain_result_t goc_pool_destroy_timeout(goc_pool* pool,
                                            uint64_t ms);    /* GOC_DRAIN_OK or GOC_DRAIN_TIMEOUT; pool remains valid on timeout */

/* Scheduler / event loop */
uv_loop_t*    goc_scheduler(void);

/* Dynamic array (see goc_array.h and ARRAY.md) */
goc_array*    goc_array_make(size_t initial_cap);
goc_array*    goc_array_from(void** items, size_t n);
size_t        goc_array_len(const goc_array* arr);
void*         goc_array_get(const goc_array* arr, size_t i);
void          goc_array_set(goc_array* arr, size_t i, void* val);
void          goc_array_push(goc_array* arr, void* val);
void*         goc_array_pop(goc_array* arr);
void          goc_array_push_head(goc_array* arr, void* val);
void*         goc_array_pop_head(goc_array* arr);
goc_array*    goc_array_concat(const goc_array* a, const goc_array* b);
goc_array*    goc_array_slice(const goc_array* arr, size_t start, size_t end);
goc_array*    goc_array_from_str(const char* s);
char*         goc_array_to_str(const goc_array* arr);
void**        goc_array_to_c(const goc_array* arr);
```

### Internal Functions

| Function | Signature | Defined in |
|---|---|---|
| `cb_queue_init` | `void cb_queue_init(void)` | `loop.c` |
| `post_to_run_queue` | `void post_to_run_queue(goc_pool* pool, goc_entry* entry)` | `pool.c` — enqueues entry for execution; does **not** touch `live_count` |
| `pool_submit_spawn` | `void pool_submit_spawn(goc_pool* pool, void (*fn)(void*), void* arg, goc_chan* join_ch)` | `pool.c` — increments `live_count`, materialises the fiber immediately or queues it behind the admission cap, and preserves eager same-pool child spawns for liveness |
| `post_callback` | `void post_callback(goc_entry* entry, void* value)` | `loop.c` |
| `on_wakeup` | `void on_wakeup(uv_async_t* handle)` | `loop.c` |
| `wake` | `bool wake(goc_chan* ch, goc_entry* entry, void* value)` | `channel.c` — returns `true` when the entry is successfully claimed and dispatched; `false` when cancelled or already claimed (via `try_claim_wake`) |
| `try_claim_wake` | `static inline bool try_claim_wake(goc_entry* e)` | `internal.h` — for `goc_alts` entries: CAS `fired` 0→1 first; if another arm already fired, returns `false`. Then CAS `woken` 0→1; returns `false` if already claimed. Returns `true` only when both CASes succeed (or `fired` is NULL for plain take/put entries). |
| `compact_dead_entries` | `void compact_dead_entries(goc_chan* ch)` | `channel.c` |
| `chan_register` | `void chan_register(goc_chan* ch)` | `gc.c` — appends `ch` to the `live_channels` array under `g_live_mutex`; called by `goc_chan_make`. This list is the sole input to the graceful-drain wait in `goc_shutdown`. |
| `chan_unregister` | `void chan_unregister(goc_chan* ch)` | `gc.c` — removes `ch` from `live_channels` under `g_live_mutex`; called by `goc_close`. |
| `chan_take_from_buffer` | `static inline int chan_take_from_buffer(goc_chan*, void**)` | `channel_internal.h` — requires `struct goc_chan` in scope via `chan_type.h` |
| `chan_take_from_putter` | `static inline int chan_take_from_putter(goc_chan*, void**)` | `channel_internal.h` — requires `struct goc_chan` in scope via `chan_type.h`; iterates the full putters queue (discarding cancelled and lost-race entries mid-queue); snapshots `e->next` before `wake()` because the parked entry may become unreachable after dispatch |
| `chan_put_to_taker` | `static inline int chan_put_to_taker(goc_chan*, void*)` | `channel_internal.h` — requires `struct goc_chan` in scope via `chan_type.h`; iterates the full takers queue (discarding cancelled and lost-race entries mid-queue); snapshots `e->next` before `wake()` because the parked entry may become unreachable after dispatch |
| `chan_put_to_buffer` | `static inline int chan_put_to_buffer(goc_chan*, void*)` | `channel_internal.h` — requires `struct goc_chan` in scope via `chan_type.h` |
| `chan_set_on_close` | `void chan_set_on_close(goc_chan* ch, void (*on_close)(void*), void* ud)` | `channel.c` (declared in `channel_internal.h`) — registers a one-shot callback invoked when the channel is closed; if the channel is already closed at call time the callback fires immediately |

> **Note:** `chan_register` and `chan_unregister` are *defined* in `gc.c` but *called* from `channel.c` (`goc_chan_make` calls `chan_register`; `goc_close` calls `chan_unregister`). The inline ring-buffer helpers live in `channel_internal.h`, which includes both `chan_type.h` and `internal.h`. Files that need channel internals should include `channel_internal.h` (or `chan_type.h` directly when only the concrete channel layout is needed).

### `goc_scheduler`

Returns the `uv_loop_t*` owned by the runtime. Must be called only after `goc_init` has returned. Once `goc_init` has returned the pointer is stable and read-only; it is safe to call from any thread at any point after that without synchronisation.

Primary use-case: registering user-owned libuv handles so they share the same event loop as the rest of the runtime.

```c
uv_tcp_t* server = malloc(sizeof(uv_tcp_t));   /* malloc — NOT GC heap */
uv_tcp_init(goc_scheduler(), server);
```

> **Do not call `uv_run` or `uv_loop_close` on the returned pointer.** The loop lifetime is managed entirely by `libgoc`.

---

## Async I/O Wrappers (`goc_io`)

libgoc provides channel-based wrappers for libuv I/O operations in a separate header (`goc_io.h`) and translation unit (`src/goc_io.c`). All public identifiers are prefixed `goc_io_`.

**Include separately:**

```c
#include "goc.h"
#include "goc_io.h"
```

**Single-form API.** Each operation is exposed as a single function that returns `goc_chan*`; the channel delivers the result when the I/O completes. Safe from any context; composable with `goc_alts()`.

**Thread-safety bridge.** Stream and UDP operations (`uv_write`, `uv_read_start`, etc.) touch handle internals that must run on the libuv loop thread. All stream/UDP wrappers (read_start/stop, write, write2, connect, shutdown, UDP send/recv, signals, TTY, FS events/polls) use `post_on_loop()`: a `GOC_CALLBACK` entry is enqueued directly into the MPSC callback queue with no per-call `uv_async_t` handle. File-system and DNS operations are submitted directly (libuv routes them through its internal thread pool).

**GC safety.** All `goc_chan*` pointers passed to async context structs (which are `malloc`-allocated) are registered in the `live_uv_handles` array (see `gc.c`) for the duration of the pending I/O, preventing premature collection.

> **Full API reference:** [IO.md](./IO.md)

---

## HTTP Server (`goc_http`)

libgoc includes a built-in HTTP/1.1 server and client built on top of `goc_io` TCP and the vendored [picohttpparser](https://github.com/h2o/picohttpparser) (MIT), integrated with the libgoc fiber scheduler.

**Include separately:** consumers must include `<goc_http.h>` in addition to `<goc.h>`.

```c
#include "goc.h"
#include "goc_http.h"
```

**Fiber-per-request model:** each HTTP request is dispatched into a new fiber via `goc_go()`, giving handlers the full co-operative concurrency model — blocking channel reads, `goc_sleep`, `goc_io` operations, and GC allocation all work transparently.

**HTTP client:** outbound requests return a `goc_chan*` that delivers a `goc_http_response_t*` when the response arrives. The event loop is never blocked; other fibers continue to run while the request is in flight. Multiple requests can be issued and awaited with `goc_alts`.

> **Planned:** WebSocket upgrades, HTTP/2, and TLS support are planned for a future release.

> **Full API reference:** [HTTP.md](./HTTP.md)

---

## Telemetry (`goc_stats`)

libgoc provides an optional, **asynchronous** telemetry system via `include/goc_stats.h` and `src/goc_stats.c`. When enabled (`-DGOC_ENABLE_STATS=ON`), the runtime emits structured events at key lifecycle points: pool creation/destruction, worker state transitions (created → running → idle → stopped), fiber creation/completion, and channel open/close.

**Delivery model.** Events are pushed by the emitting thread into a Vyukov-style MPSC queue (backed by `stats_node` elements linked through `g_sq_head` / `g_sq_tail`) and delivered to the user-supplied callback on a dedicated background **delivery loop thread** started by `goc_stats_init`. This means the callback is never invoked on the emitting thread; it always runs on the delivery thread. A default callback that prints all events to stdout is installed by `goc_stats_init`. Register a custom callback with `goc_stats_set_callback`.

**`goc_stats_flush`.** `goc_stats_flush()` blocks the calling thread until the delivery loop has drained all in-flight events from the internal queue. It must be called before inspecting or resetting test buffers to avoid a race with the async delivery thread. Example use case: in tests, call `goc_stats_flush()` after the operation under test and before asserting on the captured events.

```c
void goc_stats_flush(void);   /* Block until all queued events have been delivered */
```

**Accessor functions** (available when `GOC_ENABLE_STATS` is defined):

```c
void   goc_timeout_get_stats(uint64_t *allocations, uint64_t *expirations);
size_t goc_cb_queue_get_hwm(void);
void   goc_pool_get_steal_stats(uint64_t *attempts, uint64_t *successes,
                                uint64_t *misses,   uint64_t *idle_wakeups);
```

- `goc_timeout_get_stats` — returns cumulative timeout allocation and expiration counts.
- `goc_cb_queue_get_hwm` — returns the high-water mark of the callback queue depth.
- `goc_pool_get_steal_stats` — returns pool-wide steal counters (aggregates across all workers via the `g_steal_attempts` / `g_steal_successes` / `g_steal_misses` / `g_idle_wakeups` globals in `pool.c`):
  - `attempts` — total `wsdq_steal_top` calls (hint-path + randomised scan).
  - `successes` — attempts that returned a non-NULL entry.
  - `misses` — attempts that returned NULL; equals `attempts − successes`.
  - `idle_wakeups` — number of times a worker returned from `uv_sem_wait` (one per sleep/wake cycle). High values relative to `successes` indicate steal thrashing or spurious wakeups.

When `GOC_ENABLE_STATS` is not defined, all emission macros (`GOC_STATS_POOL_STATUS`, `GOC_STATS_WORKER_STATUS`, `GOC_STATS_FIBER_STATUS`, `GOC_STATS_CHANNEL_STATUS`) expand to `((void)0)` and have zero runtime cost.

> **Full API reference, event schema, and examples:** [TELEMETRY.md](./TELEMETRY.md)

---

## Initialization Sequence

`goc_init` must be called exactly once, as the first call in `main()`. Calling it more than once is undefined behaviour. It performs the following steps:

1. **`gc.c`** — Call `GC_INIT()`.
2. **`gc.c`** — Register the `push_fiber_roots` callback via `goc_fiber_roots_init()` / `GC_set_push_other_roots`.
3. **`gc.c`** — Call `GC_allow_register_threads()`.
4. **`gc.c`** — Initialise the `live_channels` list: a `malloc`-allocated, dynamically grown `goc_chan**` array protected by a `uv_mutex_t` (`g_live_mutex`).
5. **`gc.c`** — Call `live_uv_handles_init()`: initialise the `live_uv_handles` array that tracks GC-visible `goc_chan*` pointers held by in-flight async I/O (`timeout.c`, `goc_io.c`). This must happen before any `goc_timeout` or `goc_io_*` call.
6. **`pool.c`** — Initialise the global pool registry.
7. **`mutex.c`** — Initialise the global RW-mutex registry.
8. **`loop.c`** — Call `loop_init()`: allocate/init `g_loop`, `g_wakeup`, and `g_shutdown_async`; initialise the callback queue; then spawn the loop thread.
9. **`pool.c`** — Determine the default worker count from `GOC_POOL_THREADS` or `max(4, uv_available_parallelism())`, then call `goc_pool_make(N)` for the default pool.

---

## Shutdown Sequence

`goc_shutdown` performs orderly teardown in four steps. It must be called from outside any fiber or pool thread (e.g. the application's main thread). It must not be called concurrently with any other `libgoc` function. It will block until all fibers have completed naturally — if any fiber is waiting on a channel event that will never arrive, `goc_shutdown` will hang. After it returns, the runtime is fully torn down and further `libgoc` calls — including a second `goc_init` — are outside the supported contract.

> **Post-shutdown API boundary:** Once `goc_shutdown` returns, calling any `libgoc` function is **undefined behaviour**. Step 3 destroys and frees all channel mutexes; any subsequent call that reaches `uv_mutex_lock(ch->lock)` — including `goc_close`, `goc_take_try`, `goc_chan_make`, or any other channel API — accesses freed memory. Do not retain `goc_chan*` pointers across `goc_shutdown` with the intent to use them after the call returns.

**Step 1 — Drain all in-flight fibers.**

`goc_shutdown` iterates the global pool registry and calls `goc_pool_destroy` on every pool that has not already been destroyed — this includes the default pool and any user-created pools made with `goc_pool_make`. `goc_pool_destroy` waits on `drain_cond` (under `drain_mutex`) until `live_count` reaches zero — meaning every fiber has run to completion. Fibers are expected to finish naturally; `goc_shutdown` does not forcibly close channels or inject `ok==GOC_CLOSED` wakeups.

Once the drain-wait completes, `goc_pool_destroy`:
1. Logs a diagnostic if the run queue is somehow still non-empty.
2. Sets `pool->shutdown = 1` (atomic release store).
3. Posts one semaphore unit per worker to unblock sleeping threads.
4. Joins every worker thread.
5. Destroys the semaphore and drain primitives, drains and frees the run queue, and frees the pool.

After Step 1, no fiber or pool thread will ever call `uv_mutex_lock` on a channel lock again. However, the event loop thread may still be running timer-expiry callbacks (e.g. from `goc_timeout`) that call `goc_close()`, which also locks channel mutexes. Channel mutexes must therefore **not** be destroyed until the loop thread has been fully joined (Step 2).

**Step 2 — Shut down the event loop and join the loop thread.**

`loop_shutdown()` signals the loop thread to close both `uv_async_t` handles and then joins it. Specifically:

- `goc_shutdown` calls `uv_async_send(g_shutdown_async)` from the main thread. `uv_async_send` is the only libuv call documented as safe to call from any thread.
- The loop-thread callback `on_shutdown_signal` first drains `g_cb_queue` completely before calling `uv_close` on either handle. Although the pool is gone by this point, any entries pushed to `g_cb_queue` before the pool was destroyed and not yet flushed by a prior `on_wakeup` invocation are drained here. After draining, `on_shutdown_signal` calls `uv_close` on `g_wakeup` (with `on_wakeup_closed`) and on `g_shutdown_async` itself (with `free_handle_cb`). Both closes are performed from the loop thread, which is the only correct context: `uv_close` is not thread-safe and races against the loop's internal handle-list traversal if called from another thread.
- Once both close callbacks have fired, the loop has no remaining active handles and `loop_thread_fn` exits. The loop thread runs `while (uv_run(g_loop, UV_RUN_ONCE)) {}` for its entire lifetime — both during normal operation and during shutdown drain. `UV_RUN_ONCE` is used rather than `UV_RUN_DEFAULT` because `UV_RUN_DEFAULT` returns as soon as there are no pending callbacks in one iteration, which may be before all `uv_close` callbacks have completed; the `UV_RUN_ONCE` spin correctly drains all remaining callbacks before returning zero.
- `goc_thread_join(&g_loop_thread)` blocks until `loop_thread_fn` exits. After this: all `on_wakeup` and `on_shutdown_signal` deliveries are guaranteed complete, no libuv callback will ever fire again, and `g_loop` is safe to close and free.
- `uv_loop_close(g_loop)` is called and its return value is asserted to be `0`. Because the loop thread has exited and all handles were closed, `uv_loop_close` must succeed; a non-zero return indicates a handle was left open and is treated as a programming error. The loop is then freed and `g_loop` set to `NULL`.

Following `loop_shutdown()`, the live UV handle roots registry (`live_uv_handles`, `g_live_uv_mutex`) is torn down.

> **Why not call `uv_run` from the main thread?** libuv explicitly forbids running the same loop from two threads simultaneously — doing so causes data races on the loop's internal timer heap, handle lists, and pending queue. The correct approach is to let the loop thread's own `uv_run` drain naturally (which happens once all handles are closed), then join the thread.

**Step 3 — Destroy channel and RW-mutex internal locks.**

Safe now: the loop thread has been joined (Step 2), so no callbacks referencing channel locks are in flight. Iterate the `live_channels` list, calling `uv_mutex_destroy` and `free` on each channel's `lock` pointer, then free the list itself. Then iterate the RW-mutex registry and destroy/free each `goc_mutex` internal lock.

**Step 4 — Free fiber root chunks.**

All pools have been drained (Step 1), so no fiber holds a live slot. Iterate and free all fiber root chunk arrays, zero the bitmap, reset `fiber_root_num_chunks` to zero, and destroy `fiber_root_mutex`. This ensures a subsequent `goc_init`/`goc_shutdown` cycle starts clean.

---

## Testing

The test suite is split across phase files in `tests/`, each a self-contained C file with no external test framework dependency.

### Design

- **Harness**: `tests/test_harness.h` provides shared `TEST_BEGIN` / `ASSERT` / `TEST_PASS` / `TEST_FAIL` macros with `goto done` cleanup — no `setjmp`. All phase test files include this header instead of duplicating the macros.
- **Crash handler**: `test_harness.h` also provides `install_crash_handler()`, which registers a `SIGSEGV` and `SIGABRT` signal handler. On Linux and macOS, `sigaction` is used; on Linux the handler calls `backtrace_symbols_fd()` to print a full backtrace to `stderr` (test executables are linked with `-rdynamic` so function names resolve), and on macOS it uses `fprintf` instead of `dprintf`. On Windows, `signal()` is used as a fallback with `fprintf` to `stderr`; no backtrace is printed. In all cases the handler re-raises the signal with the default disposition restored so the process exits with the correct signal status. `install_crash_handler()` is called as the first statement of `main()` in each phase binary, before `goc_init()`. This approach is used in preference to core dumps because GitHub Actions runners do not reliably write core files to disk, whereas `stderr` output is always captured by CTest `--output-on-failure`.
- **Isolation**: `goc_init()` and `goc_shutdown()` bracket the entire test binary — called once each in the `main()` of **each phase's test binary** before and after all tests in that phase run. Individual test functions do not call them.
- **Synchronisation**: a `done_t` helper lets the main thread block until fibers finish without relying on `sleep` or a `goc_chan`. All test phases (P1–P9) use a portable `done_t` backed by `pthread_mutex_t` + `pthread_cond_t`; `done_signal` signals the condvar, `done_wait` waits on it, and `done_wait_ms` provides a timed wait. This replaces an earlier `sem_t`-based implementation that used `sem_init` / `sem_wait`, which is deprecated (and non-functional) on macOS for unnamed POSIX semaphores.
- **GC integration**: Boehm GC is linked automatically; no hook table setup is required in tests.
- **Crash tests**: tests that expect `abort()` (e.g. canary violation) use `fork` + `waitpid` in the test function itself (`fork_expect_sigabrt` helper). Each test forks a child; before calling `goc_init()` the child performs two setup steps:
  1. **Reset `SIGABRT` to `SIG_DFL`** — the parent installs a `crash_handler` for `SIGABRT` (via `install_crash_handler()`), which the child inherits through `fork`. When `abort()` fires on a pool worker thread, glibc's `abort` implementation blocks `SIGABRT` on the calling thread before raising it. The inherited crash handler then calls `raise(SIGABRT)` while the signal is still blocked, which interacts with `abort()`'s internal signal-mask loop and causes the child to hang rather than terminate. Resetting to `SIG_DFL` before `goc_init()` ensures `abort()` terminates the child immediately regardless of which thread calls it.
  2. **Force `GOC_POOL_THREADS=1`** — P8.1 requires a specific fiber execution order (victim parks before sender runs). With multiple pool workers, both fibers can be dequeued simultaneously and the ordering is not guaranteed. A single worker serialises fiber execution so the victim always parks first.

  After setup the child calls `goc_init()` from scratch (the forked address space inherits the parent's memory image but libuv handles, mutexes, and the GC's internal thread table are all in an inconsistent state because background threads were not forked). The child performs the unsafe operation and should never return — the runtime calls `abort()` before that is possible. If the child exits normally it uses `_exit(2)` so the parent can distinguish this from an abort. The parent calls `waitpid` and asserts `WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT`. The child never calls `goc_shutdown()`.

### Coverage

**Phase 1 — Foundation**

| Test | Description |
|---|---|
| P1.1 | `goc_scheduler()` returns non-NULL after `goc_init` |
| P1.2 | `goc_scheduler()` pointer is identical across repeated calls |
| P1.3 | `goc_malloc` returns non-NULL for a small allocation; result is zero-initialised |
| P1.4 | `goc_in_fiber()` returns `false` from the main thread |
| P1.5 | `goc_realloc()` grows a GC allocation preserving existing data; shrinks without corrupting surviving bytes |

**Phase 2 — Channels and fiber launch**

| Test | Description |
|---|---|
| P2.1 | `goc_chan_make(0)` returns non-NULL; is a true rendezvous channel (no buffering) |
| P2.2 | `goc_chan_make(16)` returns non-NULL; accepts up to 16 values without a taker |
| P2.3 | `goc_close` does not crash; concurrent double-close from two threads is idempotent — both calls return without error, channel state is consistent, and no mutex is double-destroyed (exercises the `close_guard` CAS path) |
| P2.4 | `goc_go` launches a fiber that runs to completion; join channel is closed on return |
| P2.5 | `goc_go` passes the arg pointer through to the fiber correctly |
| P2.6 | `goc_in_fiber()` returns `true` from within a fiber |
| P2.7 | Join channel: `goc_take_sync(done)` blocks until the fiber returns, then unblocks |
| P2.8 | Join channel: `goc_take(done)` from a second fiber suspends until the target fiber returns |

**Phase 3 — Channel I/O**

| Test | Description |
|---|---|
| P3.1 | Unbuffered rendezvous: fiber puts, main takes via `goc_take_sync` — value intact |
| P3.2 | Buffered channel: fiber fills, main drains via `goc_take_sync` — all values intact, order preserved |
| P3.3 | `goc_take_try` on an open empty channel returns `ok==GOC_EMPTY` |
| P3.4 | `goc_take_try` on a buffered channel with a value returns `ok==GOC_OK` and the correct value |
| P3.5 | `goc_take_try` on a closed channel returns `ok==GOC_CLOSED` |
| P3.6 | `goc_take_sync` on a closed empty channel returns `ok==GOC_CLOSED`; no hang |
| P3.7 | `goc_close` wakes an OS thread parked in `goc_take_sync` with `ok==GOC_CLOSED`; no hang |
| P3.8 | `goc_put_sync` delivers a value to a waiting fiber; returns `GOC_OK` |
| P3.9 | `goc_close` wakes an OS thread parked in `goc_put_sync` with `ok==GOC_CLOSED`; no hang |
| P3.10 | `goc_close` wakes a fiber parked in `goc_take` with `ok==GOC_CLOSED` |
| P3.11 | `goc_close` wakes a fiber parked in `goc_put` with `ok==GOC_CLOSED` |
| P3.12 | Buffered channel closed with values still in the ring: `goc_take_sync` drains all buffered values with `ok==GOC_OK` before returning `{NULL, GOC_CLOSED}` |
| P3.13 | Buffered channel closed with values still in the ring: fiber draining with `goc_take` receives all buffered values with `ok==GOC_OK` before receiving `{NULL, GOC_CLOSED}` |
| P3.14 | Legitimate `NULL` payload through unbuffered channel: fiber puts `NULL`, taker receives `ok==GOC_OK` and `val==NULL` |
| P3.15 | `goc_take_all_sync` with `n==0`: no crash, returns non-NULL GC array |
| P3.16 | `goc_take_all_sync` on pre-filled buffered channels: all results `ok==GOC_OK`, values intact, order matches channel order |
| P3.17 | `goc_take_all_sync` on already-closed channels: all results `ok==GOC_CLOSED` |
| P3.18 | `goc_take_all_sync` blocks until fibers send on each channel; all results `ok==GOC_OK`, values intact |
| P3.19 | `goc_take_all` from fiber on pre-filled buffered channels: all results `ok==GOC_OK`, values intact, order matches channel order |
| P3.20 | `goc_take_all` from fiber parks on each channel until a sender fiber delivers; all results `ok==GOC_OK`, values intact |

**Phase 4 — Callbacks**

| Test | Description |
|---|---|
| P4.1 | `goc_take_cb` delivers a value sent by a fiber; `ok==GOC_OK`, value intact |
| P4.2 | `goc_take_cb` fires with `ok==GOC_CLOSED` when channel is closed before delivery |
| P4.3 | `goc_put_cb` delivers a value consumed by a fiber; completion callback receives `ok==GOC_OK` |
| P4.4 | `goc_put_cb` with `cb=NULL` (fire-and-forget): value is delivered, no crash |
| P4.5 | `goc_put_cb` on a closed channel: completion callback receives `ok==GOC_CLOSED`; no hang |
| P4.6 | `goc_put_cb` with buffer space available and no parked taker: value is enqueued to the ring buffer, completion callback fires with `ok==GOC_OK`; subsequent `goc_take_sync` retrieves the correct value (exercises `goc_put_cb` step 4 — buffer path, distinct from the parked-taker path in P4.3) |

**Phase 5 — Select and timeout**

| Test | Description |
|---|---|
| P5.1 | `goc_alts` selects the immediately-ready take arm (no parking); correct index and value |
| P5.2 | `goc_alts` with a ready put arm fires immediately; result is `{NULL, GOC_OK}` with correct index |
| P5.3 | `goc_alts` parks and wakes on a take arm when no arm is immediately ready |
| P5.4 | `goc_alts` parks and wakes on a put arm when no taker is initially present |
| P5.5 | `goc_alts` with `GOC_ALT_DEFAULT`: fires immediately with correct index when no other arm is ready |
| P5.6 | `goc_alts` with `GOC_ALT_DEFAULT`: non-default arm wins when it is immediately ready; default arm does not fire |
| P5.7 | `goc_alts` on a take arm whose channel is already closed at call time: fast-path (Phase 2 non-blocking scan) returns `ok==GOC_CLOSED` with correct index immediately, without parking |
| P5.8 | `goc_alts` take arm: channel is open at call time, fiber parks (Phase 6 yield), channel is then closed by another fiber — fiber wakes with `ok==GOC_CLOSED` and correct index; no hang |
| P5.9 | `goc_alts` put arm: fiber is parked waiting for a taker, channel is then closed — fiber wakes with `ok==GOC_CLOSED` and correct index; no hang (mirrors P3.11 for the `alts` cancellation path) |
| P5.10 | `goc_alts` timeout arm fires when data channel stays empty for the deadline duration |
| P5.11 | `goc_alts_sync` blocks the OS thread on a take arm until a fiber sends a value |
| P5.12 | `goc_alts_sync` blocks the OS thread on a put arm until a fiber takes the value |
| P5.13 | `goc_timeout` closes its channel after the deadline, not before |

**Phase 6 — Thread pool + work-stealing data structures**

| Test | Description |
|---|---|
| P6.1 | `goc_pool_make` / `goc_pool_destroy` lifecycle; `goc_go_on` dispatches fiber to correct pool |
| P6.2 | `goc_pool_destroy_timeout` returns `GOC_DRAIN_OK` when all fibers finish before the deadline |
| P6.3 | `goc_pool_destroy_timeout` returns `GOC_DRAIN_TIMEOUT` when fibers are still running at deadline; pool remains valid — verified by dispatching a new short-lived fiber via `goc_go_on` to the same pool and confirming it runs to completion before `goc_pool_destroy` is called |
| P6.4 | `goc_malloc` end-to-end: fiber builds GC-heap linked list, main traverses after join |
| P6.5 | `goc_alts` with `n > GOC_ALTS_STACK_THRESHOLD` (8) arms exercises the `malloc` path in `alts_dedup_sort_channels`; correct arm fires, no memory error (run under ASAN to catch heap misuse) |
| P6.6 | `wsdq_push_bottom` / `wsdq_pop_bottom` round-trip: push 16 entries, pop verifies LIFO order |
| P6.7 | `wsdq_pop_bottom` on empty deque returns NULL |
| P6.8 | `wsdq_steal_top` on empty deque returns NULL |
| P6.9 | `wsdq_steal_top` ordering is FIFO: push 3 entries, steal verifies e1→e2→e3 |
| P6.10 | `goc_wsdq` grows when full: push 256 entries into a capacity-4 deque, pop verifies all values |
| P6.11 | Concurrent owner-push / thief-steal: 4 thief threads steal while owner pushes N entries; all N entries accounted for |
| P6.12 | Concurrent push + pop + steal under contention: owner interleaves push and pop while 4 thieves steal; all N entries accounted for |
| P6.13 | `wsdq_destroy` on non-empty deque is safe (no crash) |
| P6.14 | `injector_push` / `injector_pop` round-trip: push 64 entries, pop verifies FIFO order |
| P6.15 | `injector_pop` on empty injector returns NULL |
| P6.16 | Concurrent injector push from multiple threads: 4 producer threads push N/4 entries each; all N entries consumed with no duplicates or losses |

> **Not yet tested:** `compact_dead_entries` sweep — verifying that the amortised dead-entry sweep fires correctly when `dead_count >= GOC_DEAD_COUNT_THRESHOLD` after many fibers race on the same channel via `goc_alts`. The sweep logic exists and is exercised indirectly by the integration tests, but a dedicated test that asserts the sweep threshold, entry unlinking, and channel consistency after the sweep has not yet been written.

**Phase 7 — Integration**

| Test | Description |
|---|---|
| P7.1 | Pipeline: producer → transformer → consumer, 16 items, all values correct |
| P7.2 | Fan-out / fan-in: 1 producer, 4 workers, result aggregation, 20 items, sum verified |
| P7.3 | High-volume stress: 10 000 messages, sum verified |
| P7.4 | Multi-fiber: 8 senders on 1 unbuffered channel, all IDs received exactly once |
| P7.5 | Timeout + cancellation: slower fiber's result discarded cleanly, shutdown completes without hang. `goc_close(result_ch)` and both `goc_take_sync` fiber joins are performed **before** the result assertions so that no fiber can be left parked on an unclosed channel if a timing-sensitive assertion fails. |

**Phase 8 — Safety and crash behaviour**

| Test | Description |
|---|---|
| P8.1 | Stack overflow: an `overflow_fiber` corrupts its own canary word then parks on `goc_take`; a `sender_fiber` calls `goc_put` to wake it; `pool_worker_fn` checks the canary before the next `mco_resume`, finds it corrupted, and calls `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT`. Two fibers are used (rather than a fiber + `goc_take_sync` from the OS thread) to guarantee the victim parks before the sender runs — with `goc_take_sync`, a race exists where the OS thread parks first and the fiber's `goc_put` rendezvouses immediately without ever suspending, so the canary check never fires. **Skipped when `LIBGOC_VMEM_ENABLED` is defined** (vmem builds, `-DLIBGOC_VMEM=ON`); only exercised in canary builds (the default). |
| P8.2 | `goc_take` called from a bare OS thread (not a fiber) → `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT` |
| P8.3 | `goc_put` called from a bare OS thread (not a fiber) → `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT` |
| P8.4 | `goc_alts` called with more than one `GOC_ALT_DEFAULT` arm (from within a fiber) → `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT`. A fiber is spawned via `goc_go()` to provide fiber context. |
| P8.5 | `goc_alts_sync` called with more than one `GOC_ALT_DEFAULT` arm → `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT` |
| P8.6 | `goc_pool_destroy` called from within the target pool's own worker thread → `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT` |
| P8.7 | `goc_init` called from a non-main pthread → `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT` |
| P8.8 | `goc_shutdown` called from a non-main pthread after main-thread `goc_init` → `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT` |
| P8.9 | `goc_take_sync` called from within a fiber → `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT` |
| P8.10 | `goc_put_sync` called from within a fiber → `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT` |
| P8.11 | `goc_alts_sync` called from within a fiber → `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT` |

> **Windows:** All P8 tests self-skip on Windows. `fork()`/`waitpid()` are not available in MinGW; `test_p08_safety.c` detects `_WIN32` at compile time and emits a `TEST_SKIP` stub for each of the 11 tests. The binary builds and runs cleanly; tests report `skip` rather than failing.

**Phase 9 — RW mutexes**

| Test | Description |
|---|---|
| P9.1 | Read lock acquire+release from an OS thread: `goc_read_lock` returns a lock channel; `goc_take_sync` acquires with `ok==GOC_OK`; `goc_close` releases |
| P9.2 | Multiple readers can acquire concurrently |
| P9.3 | Writer blocks while a reader is held, then acquires after reader release |
| P9.4 | Writer preference: once a writer is queued, subsequent readers queue behind it |
| P9.5 | Fiber parking path: reader fiber blocks behind writer and resumes after writer release |

**goc_array component**

| Test | Description |
|---|---|
| `test_array_make` | `goc_array_make(0)` returns non-NULL; `goc_array_len` returns 0 |
| `test_array_push_get` | `goc_array_push` appends elements; `goc_array_get` retrieves them in order |
| `test_array_set` | `goc_array_set` overwrites an element; adjacent elements are not disturbed |
| `test_array_pop` | `goc_array_pop` removes and returns the tail element |
| `test_array_push_pop_head` | `goc_array_push_head` / `goc_array_pop_head` operate on the front; order correct after mixed head+tail pushes |
| `test_array_grow` | Array doubles capacity when full; all values survive across the realloc |
| `test_array_concat` | `goc_array_concat` produces a new array with all elements from both inputs in order |
| `test_array_slice` | `goc_array_slice` returns a view sharing the backing buffer; O(1) |
| `test_array_c_interop` | `goc_array_to_c` + `goc_array_from` round-trip: pointer into live region is valid; reconstructed array matches original |
| `test_array_to_c_empty` | `goc_array_to_c` on an empty array returns non-NULL and does not crash |
| `test_array_empty_ops` | Pop/pop_head/slice/concat on empty arrays return NULL / zero-length array without crashing |
| `test_array_from_empty` | `goc_array_from(NULL, 0)` returns a valid empty array |

**Phase 10 — Async I/O wrappers**

| Test | Description |
|---|---|
| P10.1 | `goc_io_fs_open`: open a new file; file descriptor >= 0 |
| P10.2 | `goc_io_fs_write`: write data to an open file; returns correct written byte count |
| P10.3 | `goc_io_fs_read`: read back written data; content matches |
| P10.4 | `goc_io_fs_stat`: stat the file; `st_size` equals written byte count |
| P10.5 | `goc_io_fs_rename`: rename the file; old path stat fails, new path stat succeeds |
| P10.6 | `goc_io_fs_unlink`: delete the file; subsequent stat fails |
| P10.7 | `goc_io_fs_open` with non-existent path returns negative error code |
| P10.8 | `goc_io_getaddrinfo` resolves "localhost"; `ok == GOC_IO_OK`, `res != NULL` |
| P10.9 | `goc_io_getaddrinfo` with NULL node and service: no crash |
| P10.10 | `goc_io_getaddrinfo` returns non-NULL channel |
| P10.11 | `goc_io_fs_sendfile`: copy bytes between two file descriptors; content verified |
| P10.12 | `goc_io_fs_open` integrates with `goc_alts` (select on I/O channel vs. dummy channel) |

> **Windows portability:** Temporary file paths are constructed via `GetTempPathA` on Windows and `/tmp` on POSIX. All file-system operations use `UV_FS_O_*` flags (e.g. `UV_FS_O_RDONLY`, `UV_FS_O_WRONLY | UV_FS_O_CREAT`) which are portable across all libuv platforms. Phase 10 builds and runs on Windows.

**goc_stats component** (`test_goc_stats`)

`test_goc_stats` always compiles with `GOC_ENABLE_STATS` defined and links `src/goc_stats.c` directly, independent of the `GOC_ENABLE_STATS` CMake option. This lets the telemetry tests run in any build configuration.

| Test | Description |
|---|---|
| S1.1 | `goc_stats_init` / `goc_stats_shutdown` complete without error |
| S1.2 | `goc_stats_is_enabled()` returns `true` after `goc_stats_init` |
| S1.3 | Worker event round-trips with correct `id`, `status`, and `pending_jobs` fields |
| S1.4 | Fiber event round-trips with correct `id`, `last_worker_id`, and `status` fields |
| S1.5 | Channel event round-trips with correct `id`, `status`, `buf_size`, and `item_count` fields |
| S2.1 | `goc_go` emits `GOC_FIBER_CREATED`; completion emits `GOC_FIBER_COMPLETED` |
| S2.2 | Worker transitions to `GOC_WORKER_IDLE` after fiber completes |
| S2.3 | `goc_pool_make` emits `GOC_WORKER_CREATED` per thread |
| S2.4 | Pool creation and destruction events |

### Running

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure --timeout 120
# or run a single phase directly:
./build/test_p01_foundation
```

With sanitizers:

```sh
cmake -B build-asan -DLIBGOC_ASAN=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure

cmake -B build-tsan -DLIBGOC_TSAN=ON
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

---

## CI/CD

### CI Workflow (`.github/workflows/ci.yml`)

Triggered on every push or pull request that touches `src/`, `include/`, `tests/`, `CMakeLists.txt`, `vendor/`, or the workflow file itself. Changes to documentation (`README.md`, `DESIGN.md`, `TODO.md`) do **not** trigger CI.

Runs a build matrix across four configurations:

| Runner | `cmake_flags` | Tests |
|---|---|---|
| `ubuntu-latest` | *(none — canary build)* | All phases (P1–P11), `test_goc_array`, `test_goc_stats` via `ctest --timeout 60`; P8.1 exercises canary abort |
| `macos-latest` | *(none — canary build)* | All phases (P1–P11), `test_goc_array`, `test_goc_stats` via `ctest --timeout 60`; P8.1 exercises canary abort |
| `windows-latest` | *(none — canary build)* | P1–P7, P9–P11, `test_goc_array`, `test_goc_stats` via `ctest --timeout 60`; P8 self-skips (no `fork`) |
| `ubuntu-latest` | `-DLIBGOC_VMEM=ON` (vmem build) | All phases (P1–P11), `test_goc_array`, `test_goc_stats` via `ctest --timeout 60`; P8.1 skipped (vmem build) |

All four configurations run `RelWithDebInfo` builds. Dependencies per OS:

- **Linux**: apt: `libuv1-dev`, `libatomic-ops-dev`; Boehm GC built from source with `--enable-threads=posix` and cached between runs. A `bdw-gc-threaded.pc` alias is created from the `bdw-gc.pc` file so that CMake's `pkg_check_modules(BDWGC … bdw-gc-threaded)` finds it.
- **macOS**: Homebrew: `libuv`, `bdw-gc`, `pkg-config`. Homebrew's `bdw-gc` formula does not ship a `bdw-gc-threaded.pc` pkg-config alias; the workflow creates it in `$(brew --prefix)/lib/pkgconfig`.
- **Windows**: MSYS2/MinGW-w64 (UCRT64 environment) is used instead of MSVC+vcpkg. This is required because libgoc uses `pthread.h` and C11 `_Atomic` directly. MSYS2's MinGW packages provide a POSIX-compatible toolchain with full GCC C11 support and a bdwgc build compiled with pthreads. A `bdw-gc-threaded.pc` alias is created in `/ucrt64/lib/pkgconfig` before CMake runs.

The test suite itself is portable to Windows with one exception: **Phase 8 (safety tests)** relies on `fork()`/`waitpid()` to isolate processes that call `abort()`. These POSIX APIs are not available in MinGW. `test_p08_safety.c` detects `_WIN32` at compile time and replaces each of the 11 P8 tests with a `TEST_SKIP` stub, so the binary still builds and runs cleanly — it just reports skipped tests rather than failing.

### CD Workflow (`.github/workflows/cd.yml`)

Triggered on every push to `main` that touches `src/`, `include/`, `CMakeLists.txt`, `vendor/`, or the workflow file itself. Documentation-only pushes do **not** trigger a release.

**Jobs:**

1. **`tag`** — Creates a UTC timestamp tag in the format `yyyy.MM.dd.HH.mm` (e.g. `2026.03.18.12.05`) and pushes it to the repository.

2. **`build-linux`**, **`build-macos`**, **`build-windows`** — Run in parallel after `tag`. Each job builds the `goc` static library in `Release` mode (canary stacks, the default — no `-DLIBGOC_VMEM` flag) and packages it alongside the full public header set: `include/goc.h`, `include/goc_io.h`, `include/goc_array.h`, `include/goc_stats.h`, and `include/goc_http.h`:
    - Linux: `libgoc-<tag>-canary-linux-x86_64.tar.gz` (`libgoc.a` + `goc.h` + `goc_io.h` + `goc_array.h` + `goc_stats.h` + `goc_http.h`); Boehm GC is built from source with `--enable-threads=posix` and cached; a `bdw-gc-threaded.pc` alias is baked into the cache.
    - macOS: `libgoc-<tag>-canary-macos-arm64.tar.gz` (`libgoc.a` + `goc.h` + `goc_io.h` + `goc_array.h` + `goc_stats.h` + `goc_http.h`); Homebrew dependencies are installed and a `bdw-gc-threaded.pc` alias is created in `$(brew --prefix)/lib/pkgconfig` if absent.
    - Windows: `libgoc-<tag>-canary-windows-x86_64.tar.gz` (`libgoc.a` + `goc.h` + `goc_io.h` + `goc_array.h` + `goc_stats.h` + `goc_http.h`, built via MSYS2/MinGW-w64 UCRT64); a `bdw-gc-threaded.pc` alias is created in `/ucrt64/lib/pkgconfig` if absent before CMake runs.

3. **`release`** — Downloads all three artifacts and publishes a GitHub Release tagged with the timestamp tag created in step 1.
