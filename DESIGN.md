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
17. [Initialization Sequence](#initialization-sequence)
18. [Shutdown Sequence](#shutdown-sequence)
19. [Testing](#testing)

---

## Dependencies

| | |
|---|---|
| `minicoro` | fiber suspend/resume (cross-platform; POSIX and Windows) |
| `libuv` | event loop, timers, cross-thread signalling |
| Boehm GC (bdw-gc) | garbage collection; **must be built with `--enable-threads`**; hard dependency, initialised by `goc_init`; owns thread pool worker creation via `GC_pthread_create` / `GC_pthread_join` |

---

## Repository Structure

**Not a header-only library.** libgoc requires compiled C source modules:

```
libgoc/
├── include/
│   └── goc.h              # Public API header
├── src/
│   ├── alts.c             # goc_alts, goc_alts_sync
│   ├── timeout.c          # goc_timeout
│   ├── gc.c               # Boehm GC initialization and goc_malloc
│   ├── loop.c             # libuv event loop thread
│   ├── pool.c             # Thread pool workers
│   ├── fiber.c            # minicoro fiber mechanics
│   ├── channel.c          # Channel operations
│   ├── minicoro.c         # Instantiates minicoro (defines MINICORO_IMPL)
│   ├── internal.h         # Internal types, helpers, and cross-module declarations
│   ├── chan_type.h         # Authoritative struct goc_chan definition (included by channel.c and alts.c)
│   └── config.h           # Build configuration (PAGE_SIZE, stack defaults, etc.)
├── tests/
│   ├── test_harness.h              # Shared harness macros + SIGSEGV/SIGABRT crash handler
│   ├── test_p1_foundation.c        # Phase 1 — Foundation
│   ├── test_p2_channels_fibers.c   # Phase 2 — Channels and fiber launch
│   ├── test_p3_channel_io.c        # Phase 3 — Channel I/O
│   ├── test_p4_callbacks.c         # Phase 4 — Callbacks
│   ├── test_p5_select_timeout.c    # Phase 5 — Select and timeout
│   ├── test_p6_thread_pool.c       # Phase 6 — Thread pool
│   ├── test_p7_integration.c       # Phase 7 — Integration
│   └── test_p8_safety.c            # Phase 8 — Safety and crash behaviour
├── vendor/
│   └── minicoro/
│       └── minicoro.h     # Vendored header — fiber suspend/resume (header-only)
├── CMakeLists.txt         # Build system: libgoc static lib + test binary
├── libgoc.pc.in           # pkg-config template; expanded by CMake at configure time
├── README.md
├── DESIGN.md              # This document
├── TODO.md                # Planned future work
└── LICENSE
```

minicoro is a single-header library vendored under `vendor/minicoro/`. It requires exactly one translation unit to instantiate its implementation: `src/minicoro.c` defines `MINICORO_IMPL` before including `minicoro.h`. All other files include `minicoro.h` without defining `MINICORO_IMPL`.

`src/minicoro.c` **must be compiled without `-DGC_THREADS`** (see [minicoro Limitations](#minicoro-limitations)). `CMakeLists.txt` enforces this via `set_source_files_properties(src/minicoro.c PROPERTIES COMPILE_OPTIONS "-UGC_THREADS")`.

---

## Build System Setup

The project uses CMake (≥ 3.20). `CMakeLists.txt` defines the following primary targets:

| Target | Type | Description |
|---|---|---|
| `goc` | static library | All `src/*.c` modules; always built |
| `goc_shared` | shared library | Same sources as `goc`; built only with `-DLIBGOC_SHARED=ON`; output name `libgoc.so` / `.dylib` / `.dll` |
| `test_p1_foundation` … `test_p8_safety` | executables | One per phase, discovered via `file(GLOB tests/test_p*.c)`; each linked against the active `goc` variant + libuv + Boehm GC |

A CMake function `goc_configure_target(<target>)` centralises the options shared by every library variant: `PUBLIC` include path `include/`, `PRIVATE` paths `src/` and `vendor/minicoro/`, compile definition `GC_THREADS`, and link libraries `PkgConfig::LIBUV` and `PkgConfig::BDWGC`. All library targets (`goc`, `goc_shared`, `goc_asan`, `goc_tsan`) are configured through this function.

Dependencies are resolved via `pkg-config` (libuv as `libuv`, Boehm GC as `bdw-gc-threaded` — **no fallback**; configure fails loudly if the threaded variant is absent). minicoro is instantiated via `src/minicoro.c` (which defines `MINICORO_IMPL`) and its header is available to all targets via `target_include_directories` pointing at `vendor/minicoro/`.

> **Boehm GC thread registration:** libgoc compiles with `-DGC_THREADS` and requires a Boehm GC built with `--enable-threads` (the `bdw-gc-threaded` pkg-config module). Linking against a non-threaded build causes `GC_INIT()` to malfunction or segfault at runtime; CMake will fail at configure time if the threaded variant is absent. See `README.md` for per-platform installation instructions. At runtime, all threads — pool workers and the uv loop thread — are created via `GC_pthread_create`, which wraps the thread start with `GC_call_with_stack_base` so each thread is automatically registered with the collector. **No thread created by libgoc calls `GC_register_my_thread` / `GC_unregister_my_thread` manually** — doing so double-registers the thread, corrupts the GC's internal thread table, and produces a SIGSEGV inside `GC_call_with_stack_base` on thread startup (observed as a crash during P1.4).

Named constants defined in `config.h`:
- `GOC_PAGE_SIZE`
- `GOC_DEFAULT_STACK_SIZE 65536` (64 KB per spec)
- `GOC_DEAD_COUNT_THRESHOLD 8`
- `GOC_ALTS_STACK_THRESHOLD 8` (maximum number of `goc_alts` arms for which the channel-pointer scratch buffer is stack-allocated rather than `malloc`-allocated; avoids a heap allocation for the common case of a small select)

Optional opt-in flags, each requiring a **separate build directory**:

| CMake flag | Extra target(s) | Notes |
|---|---|---|
| `-DLIBGOC_SHARED=ON` | `goc_shared` | Shared library variant; installed by `install(TARGETS goc_shared …)` |
| `-DLIBGOC_ASAN=ON` | `goc_asan` | AddressSanitizer; per-phase test executables link against `goc_asan`; mutually exclusive with TSAN and COVERAGE |
| `-DLIBGOC_TSAN=ON` | `goc_tsan` | ThreadSanitizer; per-phase test executables link against `goc_tsan`; mutually exclusive with ASAN and COVERAGE |
| `-DLIBGOC_COVERAGE=ON` | `coverage` target (if lcov/genhtml found) | Instruments `goc` with `--coverage`; runs ctest then produces `coverage_html/index.html`; mutually exclusive with ASAN and TSAN |

ASAN and TSAN each compile a separate instrumented copy of the `goc` static library (`goc_asan` / `goc_tsan`) so that sanitizer flags propagate through all object files. When either sanitizer flag is active the per-phase test executables link against the instrumented variant instead of `goc`. Configuring ASAN and TSAN together, or either sanitizer with COVERAGE, in the same directory is a CMake fatal error.

**Install rules** (`cmake --install`): `libgoc.a` is installed to `${CMAKE_INSTALL_LIBDIR}`, `include/goc.h` to `${CMAKE_INSTALL_INCLUDEDIR}`, and (when `-DLIBGOC_SHARED=ON`) the shared library to `${CMAKE_INSTALL_LIBDIR}` / `${CMAKE_INSTALL_BINDIR}`. A `pkg-config` file is generated from `libgoc.pc.in` at configure time and installed to `${CMAKE_INSTALL_LIBDIR}/pkgconfig/libgoc.pc`.

> **Static archive link order:** On Linux, `ld` processes archives in a single left-to-right pass and only extracts an object file when it resolves a symbol already pending. Within `libgoc.a` there are upward cross-references: `gc.c.o` calls into `pool.c.o`, and `channel.c.o` / `fiber.c.o` call `post_to_run_queue` in `pool.c.o`. To resolve these regardless of object file ordering, every test executable links against `libgoc.a` wrapped in `-Wl,--start-group … -Wl,--end-group`, which instructs `ld` to rescan the enclosed archives until all cross-references are satisfied. `cmake_path` (required by the test discovery loop) sets the effective minimum at CMake 3.20; the `$<LINK_GROUP:RESCAN,...>` generator expression (CMake ≥ 3.24) is not used to preserve compatibility with the stated minimum.

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

Pool threads execute fibers. When a timer fires or an external event completes, the uv loop thread calls `uv_async_send` to wake a pool thread, which resumes the parked fiber.

The pool threads and the event loop thread are **separate**. Fibers run on pool threads. The event loop thread only drives timers and dispatches wakeups — it never runs fiber code.

---

## libuv Role

libuv owns **one thing**: the event loop thread.

| libuv primitive | used for |
|---|---|
| `uv_loop_t` | single event loop, runs on its own thread |
| `uv_async_t` | wake the loop from pool threads (fiber resume signal) |
| `uv_timer_t` | `goc_timeout` implementation |

**All libuv handles (`uv_timer_t`, `uv_async_t`, etc.) must be allocated outside the GC heap** (e.g. plain `malloc`). libuv holds internal references to handles until `uv_close` completes; GC-allocated handles risk collection before `uv_close` fires, causing use-after-free inside the event loop.

This rule applies equally to **user-created handles** registered on the loop via `goc_scheduler()`. Any `uv_tcp_t`, `uv_udp_t`, `uv_pipe_t`, `uv_fs_event_t`, or other handle the caller initialises with `uv_*_init(goc_scheduler(), handle)` must be `malloc`-allocated. Free it only inside the `uv_close` completion callback:

```c
static void on_handle_closed(uv_handle_t* h) {
    free(h);   /* safe: uv guarantees no further callbacks after this point */
}

/* to tear down: */
uv_close((uv_handle_t*)my_handle, on_handle_closed);
```

Never pass a stack-allocated or GC-heap-allocated handle to any `uv_*_init` function.

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

`goc_init` must be **the first call in `main()`**, before any other library or application code that could trigger GC allocation. It calls `GC_INIT()` unconditionally as its very first operation, followed immediately by `GC_allow_register_threads()` — `GC_INIT()` performs all one-time GC setup, and `GC_allow_register_threads()` enables multi-thread stack registration. **Callers must not call these functions themselves.**

All threads — pool workers and the uv loop thread — are created via `GC_pthread_create`, which wraps the thread start with `GC_call_with_stack_base` so each thread is automatically registered with the collector before its body executes. No thread must call `GC_register_my_thread` / `GC_unregister_my_thread` manually — doing so double-registers the thread and corrupts the GC's internal thread table.

**`goc_init` must be called exactly once, as the very first call in `main()`, before any other library or application code that could trigger GC allocation. Calling it more than once is undefined behaviour.**

`goc_malloc(n)` is the public allocator. It is a thin wrapper around `GC_malloc`. Memory is zero-initialised and collected automatically when no longer reachable — no `free` is required or permitted.

**libuv handles must still be `malloc`-allocated** (not `goc_malloc`) — see [libuv Role](#libuv-role). The library's own internal structures also use plain `malloc` for any object whose lifetime is explicitly managed and does not need to participate in GC traversal.

All other GC-managed objects (channels, fibers, entries, ring buffers) are allocated via `goc_malloc` so that pointers inside them are visible to the collector automatically.

---

## Data Structures

All GC-managed structs are allocated via `goc_malloc` — on the GC heap, so pointers inside them are visible to the collector automatically.

**Exception:** libuv handles (`uv_timer_t`, `uv_mutex_t` internals, `uv_async_t`) are allocated with plain `malloc` and freed explicitly. See [libuv Role](#libuv-role) above.

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

Returned by `goc_take`, `goc_take_sync`, `goc_take_try`, and the `value` field of `goc_alts_result`. `ok==GOC_CLOSED` always means the channel was closed, regardless of `val`. `ok==GOC_EMPTY` is returned only by `goc_take_try` when the channel is open but has no value immediately available.

### Channel (`goc_chan`)

The full struct definition lives in `src/chan_type.h` — the single authoritative location. `channel.c` and `alts.c` include `chan_type.h` directly (via `internal.h`, which includes it). All other files treat `goc_chan` as an opaque pointer declared in `goc.h`.

```c
/* src/chan_type.h */
struct goc_chan {
    void**       buf;         /* ring buffer, GC-allocated */
    size_t       buf_size;
    size_t       buf_head;
    size_t       buf_count;
    goc_entry*   takers;      /* linked list */
    goc_entry*   putters;
    uv_mutex_t*  lock;        /* malloc-allocated; destroyed by goc_shutdown after pool drain */
    int          closed;
    size_t       dead_count;  /* cancelled entries still physically on takers/putters lists */
    _Atomic int  close_guard; /* 0 = open; CAS 0→1 to become the sole teardown owner */
};
```

`lock` is a `uv_mutex_t*` allocated with `malloc` in `goc_chan_make` and initialised with `uv_mutex_init`. **Mutex teardown is deferred — it is not performed inside `goc_close`.** The mutex is destroyed and freed by `goc_shutdown` in Step 2b, after all pool threads have been joined and no code path can ever call `uv_mutex_lock(ch->lock)` again. See [Shutdown Sequence](#shutdown-sequence).

**`goc_close` uses an atomic CAS on `close_guard` to serialise concurrent closers:**

1. **CAS:** attempt to flip `close_guard` from `0` to `1` with `acq_rel` ordering.
2. **Loser path:** if the CAS fails, another caller already owns teardown — return immediately. No pointer is read, no lock is attempted.
3. **Winner path:** acquire `ch->lock`, set `ch->closed = 1`, wake all parked takers and putters with `ok==GOC_CLOSED`, release the lock, call `chan_unregister`. **The mutex is left valid and intact** — ownership of `ch->lock` passes to the shutdown sweep in Step 2b.

   When iterating takers and putters, `goc_close` must skip any entry where `e->cancelled == 1` **before** attempting the `woken` CAS. Cancelled entries belong to `goc_alts` losers whose coroutine is already `MCO_DEAD`; winning the CAS on such an entry and calling `post_to_run_queue` would call `mco_resume` on a dead coroutine, producing a SIGSEGV. This is the same guard applied by `wake()` and must be kept consistent with it.

   See [Unified Wakeup](#unified-wakeup) for the `e->next` snapshot requirement.

> **Why not destroy the mutex inline?** After `goc_close` wakes parked fibers, those fibers are re-enqueued on pool threads and will resume shortly. A resumed fiber may call `goc_take` or `goc_put` on the same channel pointer — the first thing those functions do is `uv_mutex_lock(ch->lock)`. If `goc_close` had already freed the mutex, that call would crash. The mutex must remain valid until the pool is fully drained and all workers are joined (Step 2). Only at that point is it guaranteed that no fiber can reach the channel's lock again.

### Parked Entry (`goc_entry`)

```c
typedef enum { GOC_FIBER, GOC_CALLBACK, GOC_SYNC } goc_entry_kind;

typedef struct goc_entry {
    goc_entry_kind    kind;
    _Atomic int       cancelled;    /* alts: skip if set; written/read from multiple pool threads */
    _Atomic int       woken;        /* set by wake before dispatch; guards against double-wake */
    _Atomic int*      fired;        /* shared flag for goc_alts; NULL for plain take/put entries */
    struct goc_entry* next;
    goc_pool*         pool;         /* pool to re-enqueue this fiber on after a channel wake */

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
    sem_t             sync_sem;     /* embedded semaphore for single-entry sync callers */
    sem_t*            sync_sem_ptr; /* pointer used by wake(); always &sync_sem for plain
                                       goc_take_sync / goc_put_sync; set to a shared stack
                                       sem for goc_alts_sync arms so all arms post the same
                                       semaphore without knowing whether it is embedded or shared */
} goc_entry;
```

---

## Fiber Mechanics

### Stack Management

Each fiber stack is created and managed by minicoro. Stack allocation, alignment, and guard-page setup are handled internally by minicoro — libgoc does not call `posix_memalign` or `mprotect` directly for fiber stacks.

Per-fiber stack size is fixed at the default defined in `config.h` (`GOC_DEFAULT_STACK_SIZE`, 64 KB). Avoid large stack allocations and deep recursion inside fiber entry functions — see [minicoro Limitations](#minicoro-limitations).

**Canary-based stack overflow detection.** libgoc writes a sentinel canary word at the lowest address of each fiber stack immediately after `mco_create` returns. The worker loop checks the canary *before* every `mco_resume` call. If the canary has been overwritten, the runtime calls `abort()` with a diagnostic message identifying the corrupted fiber. This converts the silent heap corruption that minicoro would otherwise allow into a deterministic, debuggable crash. The canary is stored in `goc_entry` alongside the coroutine handle:

```c
/* written once in fiber.c immediately after mco_create */
entry->stack_canary_ptr = (uint32_t*)entry->coro->stack_base;
*entry->stack_canary_ptr = GOC_STACK_CANARY;   /* e.g. 0xDEADC0DE */

/* checked in pool.c before every mco_resume */
if (*entry->stack_canary_ptr != GOC_STACK_CANARY)
    abort();   /* stack overflow detected — terminate immediately */
mco_resume(entry->coro);
```

```c
goc_chan* goc_go(void (*fn)(void*), void* arg);
goc_chan* goc_go_on(goc_pool*, void (*fn)(void*), void* arg);
```

`goc_go` posts to the default pool. `goc_go_on` posts to a specific pool. Both return a join channel — see [Join Channel](#join-channel) below. Multiple pools allow priority separation or affinity pinning.

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

/* Increment live_count exactly once for this new fiber before queuing it.
   pool_fiber_born must precede post_to_run_queue so that live_count is
   non-zero before the worker could potentially see MCO_DEAD and decrement. */
pool_fiber_born(pool);
post_to_run_queue(pool, entry);
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
goc_alt_op ops[] = {
    { .ch = done,              .op_kind = GOC_ALT_TAKE },
    { .ch = goc_timeout(5000), .op_kind = GOC_ALT_TAKE },
};
goc_alts_result r = goc_alts(ops, 2);
```

The join channel may be ignored if no join is needed — the channel is GC-allocated and will be collected once it becomes unreachable after the fiber closes it.

### Suspend / Resume

Fibers suspend by calling `mco_yield` and are resumed by the pool worker calling `mco_resume`. The GC root window for the fiber's stack must remain live for the **entire suspension window** — from the moment the fiber parks until it is resumed. Roots are added before yield and removed after resume on the worker side.

```c
/* inside goc_take / goc_put, on a pool thread */
GC_add_roots(stack_base, stack_base + stack_size);
mco_yield(coro);   /* suspend — fiber stack now a GC root */

/* --- fiber is parked here; GC may run --- */

/* resume — mco_resume returns here when fiber is rescheduled */
GC_remove_roots(stack_base, stack_base + stack_size);
/* registers are live again; stack no longer needs to be an explicit root */
```

> **`goc_entry` as a GC root across fiber exit:** The `goc_entry` itself (not just the stack) must remain a GC root from fiber launch until *after* the fiber's final resume returns on the scheduler side. To close this window, the scheduler loop holds `goc_entry* entry` as a **local variable on the scheduler stack** and calls `GC_add_roots(&entry, &entry + 1)` *before* `mco_resume`. `GC_remove_roots` is called only *after* `mco_resume` returns.

---

## minicoro Limitations

libgoc uses [minicoro](https://github.com/edubart/minicoro) for all fiber switching. Three hard constraints apply to all fiber entry functions and must be understood by library embedders:

**C++ exceptions are not supported.** The C++ exception mechanism maintains internal unwinding state that is not preserved across a coroutine context switch. Throwing an exception that propagates across a `mco_yield` / `mco_resume` boundary is undefined behaviour and will typically corrupt the exception handler chain or crash. In mixed C/C++ codebases all fiber entry functions must be declared `extern "C"` and must not allow any C++ exception to escape them.

**Stack overflow aborts the process.** libgoc writes a canary value at the low end of each fiber stack on creation and validates it on every resume. If the canary has been overwritten, the runtime calls `abort()` immediately with a diagnostic message. This turns silent heap corruption into a deterministic, debuggable crash. Stack overflow is still a programming error — avoid large stack-allocated buffers and deep recursion inside fibers. If a fiber is known to need more stack space, restructure the work to use `goc_malloc`-allocated buffers on the GC heap instead.

**`src/minicoro.c` must be compiled without `-DGC_THREADS`.** minicoro declares a `static MCO_THREAD_LOCAL mco_current_co` variable (a `thread_local` TLS slot) to track the running coroutine per thread. When Boehm GC is compiled with `GC_THREADS` it wraps `pthread_create` and invokes `GC_call_with_stack_base` during thread startup, which walks TLS descriptors. If minicoro's TLS block is visible to that walk before `mco_current_co` is initialised on a new thread, the GC faults with a SIGSEGV inside its own startup code — even if no minicoro function has yet been called. The fix is to compile `src/minicoro.c` in its own isolated translation unit with `-UGC_THREADS`, which `CMakeLists.txt` enforces via `set_source_files_properties`. minicoro never calls any GC function, so the flag is irrelevant to its correctness regardless.

---

## Channel Operation Flow

### `goc_take(ch)` — fiber context only

> **Must only be called from within a fiber.** The slow path calls `mco_running()` and asserts the result is non-NULL; calling `goc_take` from a bare OS thread aborts with a diagnostic message rather than silently crashing inside `GC_add_roots`.

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
    allocate goc_entry (FIBER)
    /* current_pool = ((goc_entry*)mco_get_user_data(mco_running()))->pool */
    entry.stack_canary_ptr = mco_running()->stack_base   /* required: pool worker checks this on re-resume */
    push onto ch->takers
    GC_add_roots(stack)
    uv_mutex_unlock
    mco_yield → suspend

    /* resumes here */
    GC_remove_roots(stack)
    return {*result_slot, entry->ok}
```

`goc_put` is the exact mirror, with the same fiber-context assert, the same `stack_canary_ptr` initialization, and the same GC root registration (`GC_add_roots` / `GC_remove_roots`) in its slow path. The `current_pool` retrieval is identical. Do not omit either `stack_canary_ptr` or GC root registration from `goc_put` — the pool worker dereferences `stack_canary_ptr` on every resume (including re-resumes after a channel wake), and the fiber's stack must remain a GC root for the entire duration it is parked.

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

Blocking calls for non-fiber threads (e.g. a plain `pthread` that needs to rendezvous with fiber code). Internally parks a semaphore rather than a fiber context; the calling OS thread blocks until a value is available. Must not be called from a fiber — use `goc_take`/`goc_put` there. Must not be called from a libuv callback — it would deadlock the event loop.

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
    allocate goc_entry (SYNC), embed semaphore
    push onto ch->putters
    uv_mutex_unlock
    sem_wait(entry->sync_sem)   /* block OS thread */

    /* resumes here */
    return entry->ok            /* GOC_OK = delivered, GOC_CLOSED = channel closed */
```

### `goc_take_try(ch)` — non-blocking, any context

Non-blocking attempt to receive from a channel. Returns immediately under lock with one of four outcomes: a buffered value (`GOC_OK`), a value from a parked putter (`GOC_OK`), closed-and-empty (`GOC_CLOSED`), or open-but-empty (`GOC_EMPTY`). Never parks.

`goc_take_try` does **not** trigger `compact_dead_entries`. It is a non-blocking fast path; compaction is left to the next `goc_take` or `goc_put` call on the same channel.

### Unified Wakeup

```c
void wake(goc_chan* ch, goc_entry* e, void* value) {
    /* acquire ordering: see the full write that set `cancelled` before deciding to skip */
    if (atomic_load_explicit(&e->cancelled, memory_order_acquire)) {
        ch->dead_count++;   /* entry stays on the list; compaction runs at next take/put */
        return;
    }
    /* acq_rel CAS on woken guards against two channels simultaneously waking the same
       entry (possible in goc_alts) before cancelled has propagated. Only the CAS winner
       proceeds; the loser treats the entry as already handled. */
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &e->woken, &expected, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        ch->dead_count++;
        return;
    }
    e->ok = GOC_OK;
    if (e->kind == GOC_FIBER) {
        *e->result_slot = value;
        post_to_run_queue(e->pool, e);
    } else if (e->kind == GOC_CALLBACK) {
        post_callback(e, value);   /* posted via uv_async_send; cb fires on loop thread */
    } else {
        /* GOC_SYNC: store result and unblock the waiting OS thread */
        *e->result_slot = value;
        sem_post(e->sync_sem_ptr);
    }
}
```

At the **top of `goc_take` and `goc_put`**, immediately after acquiring the lock, if `dead_count >= GOC_DEAD_COUNT_THRESHOLD`, walk both `takers` and `putters` and unlink every entry where `cancelled == 1`, then reset `dead_count` to 0. This amortises list cleanup without requiring inline removal at cancellation time.

> **`e->next` must be saved before calling `wake()` in the splice helpers, and before any dispatch call in `goc_close`.** `chan_put_to_taker` and `chan_take_from_putter` both splice the woken entry out of its list with `*pp = e->next`. For `GOC_FIBER` entries, `wake()` calls `post_to_run_queue`, which enqueues the entry on the pool's run queue. On a multi-core system a pool thread may dequeue and resume the fiber *immediately* — before `wake()` even returns. That fiber's `mco_yield` call site resumes and the function continues to its return, deallocating the stack frame that contains the `goc_entry` (stack-allocated in `goc_take`/`goc_put` slow paths). Any subsequent read of `e->next` would be a use-after-free and a potential SIGSEGV. The fix is to snapshot `next = e->next` *before* calling `wake()`, while `ch->lock` is still held (the lock prevents concurrent modification of the list), then use `next` for the splice after `wake()` returns. The same hazard applies to `goc_close`, which iterates `ch->takers` and `ch->putters` and calls `post_to_run_queue` directly. For `goc_alts`-parked entries the `goc_entry` is GC-heap allocated, so a resumed fiber can allow the GC to collect the entry while `goc_close`'s loop is still running — `e->next` after dispatch is equally a use-after-free. `goc_close`'s loops are therefore written as `while (e != NULL) { goc_entry* next = e->next; ..dispatch..; e = next; }`. This pattern must be preserved in any future modification of these helpers and of `goc_close`.

> **Why not `dead_count > buf_count + GOC_DEAD_COUNT_THRESHOLD`?** Scaling the threshold by `buf_count` would delay compaction dramatically on heavily-buffered channels — a channel with 1 024 buffered slots would accumulate 1 032 dead entries before a sweep. Comparing against a fixed threshold bounds the maximum number of stale entries regardless of buffer capacity.

---

## Thread Pool

```c
typedef struct goc_pool {
    pthread_t*      threads;
    size_t          thread_count;
    goc_runq        runq;           /* two-lock MPMC queue of goc_entry* */
    uv_sem_t        work_sem;       /* signalled when runq becomes non-empty */
    _Atomic int     shutdown;       /* set to 1 to stop workers */

    /* Drain synchronisation — used by goc_pool_destroy to wait until all
       in-flight fibers have completed before joining worker threads. */
    pthread_mutex_t drain_mutex;
    pthread_cond_t  drain_cond;
    size_t          active_count;   /* fibers currently queued or executing; incremented
                                       by post_to_run_queue, decremented unconditionally
                                       after every mco_resume (yield or exit).  Used for
                                       internal scheduling accounting only. */
    size_t          live_count;     /* fibers still alive on this pool; incremented exactly
                                       once per fiber at birth by pool_fiber_born() (called
                                       from goc_go_on before post_to_run_queue), decremented
                                       only when mco_status == MCO_DEAD.  This is the correct
                                       drain signal: a parked fiber (MCO_SUSPENDED) still has
                                       live_count > 0 even though active_count has already
                                       been decremented to 0.
                                       NOTE: post_to_run_queue does NOT increment live_count.
                                       Re-queuing a parked fiber via wake() must not inflate
                                       it, or the count would grow unboundedly with channel
                                       operations and goc_pool_destroy would never unblock. */
} goc_pool;
```

> **`drain_cond` is signalled only on `MCO_DEAD`.** `active_count` is decremented on every yield or exit, but the broadcast fires only when a fiber actually dies. `goc_pool_destroy` and `goc_pool_destroy_timeout` wait on `live_count == 0`, not `active_count == 0` — a parked fiber (MCO_SUSPENDED) has already decremented `active_count` to zero while still being alive, so using `active_count` as the drain signal would cause a premature return while fibers are merely blocked on a channel.

The default pool is created by `goc_init` with `max(4, hardware_concurrency)` worker threads. This can be overridden by setting the `GOC_POOL_THREADS` environment variable before calling `goc_init`.

Each worker thread runs a loop:

```
while not shutdown:
    wait on work_sem
    goc_entry* entry = runq_pop()
    GC_add_roots(&entry, &entry + 1)   /* entry reachable for entire mco_resume window */
    if *entry->stack_canary_ptr != GOC_STACK_CANARY:
        abort()                        /* stack overflow — deterministic crash, not silent corruption
                                          applies to ALL entry kinds: launch entries AND goc_alts
                                          park entries; stack_canary_ptr must never be NULL */
    mco_coro* coro = entry->coro       /* snapshot handle before resume — see note below */
    mco_resume(coro)                   /* run fiber until next mco_yield or return */
    GC_remove_roots(&entry, &entry + 1)
    lock(drain_mutex); active_count--; unlock(drain_mutex)
    /* active_count is decremented unconditionally.  The lock is released before
       checking MCO_DEAD to avoid holding drain_mutex across mco_destroy. */
    if mco_status(coro) == MCO_DEAD:
        mco_destroy(coro)
        lock(drain_mutex); live_count--; broadcast(drain_cond); unlock(drain_mutex)
    /* if MCO_SUSPENDED: the fiber yielded and is parked on a channel.
       active_count has been decremented but live_count has NOT — the fiber is
       still alive.  wake() → post_to_run_queue() will re-increment active_count
       only (not live_count) when the fiber is rescheduled.  drain_cond is not
       broadcast here: live_count > 0 still holds, so goc_pool_destroy /
       goc_pool_destroy_timeout will not see a spurious drain-complete. */
```

> **Why `coro` is snapshotted before `mco_resume`.** When a fiber suspends inside `goc_take`, it stack-allocates a `goc_entry` (the parking entry) on its own coroutine stack and parks. The pool worker's `entry` pointer therefore points into the fiber's stack for the duration of that park. If the fiber is immediately re-scheduled on the same resume call (i.e. it runs to completion without yielding again), minicoro recycles the stack — and the memory `entry` pointed at is gone by the time `mco_resume` returns. Reading `entry->coro` after the resume is therefore a use-after-free and a potential segfault. The `mco_coro` object itself is minicoro heap-allocated and remains valid until `mco_destroy`, so snapshotting `coro = entry->coro` *before* the resume is always safe and eliminates the hazard entirely. See [Unified Wakeup](#unified-wakeup) for the analogous `e->next` snapshot requirement in `chan_put_to_taker` and `chan_take_from_putter`.

The invariant is:

- `pool_fiber_born` increments `live_count` exactly once per fiber, immediately before `post_to_run_queue` in `goc_go_on`.
- `post_to_run_queue` increments **only** `active_count` before pushing to the run queue. It does **not** touch `live_count` — re-queuing a parked fiber must not inflate it.
- After `mco_resume` returns, the worker always decrements `active_count` — whether the fiber yielded (`MCO_SUSPENDED`) or exited (`MCO_DEAD`).
- `live_count` is decremented **only** when `mco_status(coro) == MCO_DEAD`. `drain_cond` is broadcast at the same time.
- `goc_pool_destroy` and `goc_pool_destroy_timeout` wait on `live_count == 0`. This correctly waits for all fibers to exit, not merely to yield.

`goc_pool_destroy` is a **blocking drain-and-join**. It waits in a loop on `drain_cond` (under `drain_mutex`), re-checking `live_count > 0` on each wake to guard against spurious wakeups, until `live_count` reaches zero, meaning every fiber launched on this pool has actually returned. Only then does it signal `shutdown = 1`, post the semaphore to unblock sleeping workers, and join all threads. It is safe to call `goc_pool_destroy` while fibers are still queued, running, or parked — the call is itself the synchronisation barrier.

`goc_pool_destroy` must **not** be called from within one of the target pool's own worker threads (including from a fiber running on that pool). That self-destroy path would attempt to join the calling thread and deadlock; the implementation detects this and calls `abort()` with a diagnostic message.

`goc_pool_destroy_timeout(pool, ms)` is the non-blocking variant. It waits on `drain_cond` with a deadline of `ms` milliseconds. If `live_count` reaches zero before the deadline it performs the same shutdown-and-join sequence as `goc_pool_destroy` and returns `GOC_DRAIN_OK`. If the deadline expires while `live_count > 0` it returns `GOC_DRAIN_TIMEOUT` immediately — **the pool is not destroyed** and remains fully valid. Worker threads continue executing. The caller may retry `goc_pool_destroy_timeout`, call `goc_pool_destroy` for an unconditional wait, or close the channels that parked fibers are blocked on to unblock them before retrying.

`goc_pool_destroy_timeout` has the same self-call restriction: invoking it from a worker thread that belongs to `pool` is invalid and aborts with a diagnostic message.

> **Scope:** `live_count` only tracks fibers on *this specific pool*. Fibers on other pools (including the default pool) are not counted. `goc_pool_destroy` will block indefinitely if any fiber **on this pool** is parked on a channel event that will never arrive — for example, if a fiber launched via `goc_go_on(this_pool, ...)` is waiting on a channel that nothing will ever write to.



`runq` is a **two-lock MPMC linked queue** (Michael & Scott 1996) with a `head_lock` on the consumer side and a `tail_lock` on the producer side. Any thread (pool or loop) may push; only pool worker threads pop.

```c
typedef struct goc_runq_node {
    goc_entry*           entry;
    struct goc_runq_node* next;   /* GC-allocated node */
} goc_runq_node;

typedef struct {
    goc_runq_node* head;       /* consumer end; protected by head_lock */
    goc_runq_node* tail;       /* producer end; protected by tail_lock */
    uv_mutex_t     head_lock;
    uv_mutex_t     tail_lock;
} goc_runq;
```

```
runq_push(q, entry):
    node = goc_malloc(sizeof(goc_runq_node))
    node->entry = entry; node->next = NULL
    uv_mutex_lock(&q->tail_lock)
    q->tail->next = node
    q->tail = node
    uv_mutex_unlock(&q->tail_lock)

runq_pop(q) → goc_entry* or NULL:
    uv_mutex_lock(&q->head_lock)
    node = q->head->next          /* head is a sentinel */
    if node == NULL:
        uv_mutex_unlock(&q->head_lock) → return NULL
    q->head = node
    entry = node->entry
    uv_mutex_unlock(&q->head_lock)
    return entry
```

All threads — pool workers and the uv loop thread — are created via `GC_pthread_create` and are automatically registered with Boehm GC. No thread calls `GC_register_my_thread` / `GC_unregister_my_thread` manually.

---

## Cross-Thread Wakeup via `uv_async_t`

When a pool thread wakes a callback entry, it needs to post work back safely. A shared `uv_async_t` (malloc-allocated) handles this:

```c
/* init */
uv_async_init(loop, &wakeup_handle, on_wakeup);

/* pool thread: enqueue callback entry, then signal loop */
mpsc_push(&callback_queue, entry);
uv_async_send(&wakeup_handle);

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

> **`post_callback` guards against calling `uv_async_send` on a closed handle using an atomic acquire/release pair.** `g_wakeup` is declared `_Atomic(uv_async_t *)`. `goc_init` stores the handle pointer with `memory_order_release` after `uv_async_init` completes. `on_wakeup_closed` stores `NULL` with `memory_order_release` once the handle is fully closed. `post_callback` reads with `memory_order_acquire`, guaranteeing it either sees a fully-initialised handle or NULL — never a partial or stale pointer. A pool worker woken by `goc_shutdown`'s Step 1 `goc_close` loop may call `post_callback` after the handle is closed; the acquire load returns NULL and the send is skipped. The entry is already safely enqueued; `on_shutdown_signal` drains the queue before the loop exits:
>
> ```c
> void post_callback(goc_entry* e, void* value) {
>     /* ... push to g_cb_queue ... */
>     uv_async_t* wakeup = atomic_load_explicit(&g_wakeup, memory_order_acquire);
>     if (wakeup)
>         uv_async_send(wakeup);
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

`uv_timer_t` is allocated with plain `malloc` (not `goc_malloc`) — see [libuv Role](#libuv-role). The handle is freed in the `uv_close` callback.

`uv_timer_init` and `uv_timer_start` are **not thread-safe**; they must be called from the loop thread. Because `goc_timeout` may be called from any thread (fiber, pool worker, main thread), the timer initialisation is marshalled onto the loop thread via a one-shot `uv_async_t` embedded in a heap-allocated `goc_timeout_req`:

```c
typedef struct {
    uv_async_t  async;       /* MUST be first — cast between async* and req* */
    goc_chan*    ch;
    uv_timer_t* t;
    uint64_t    ms;
    uint64_t    deadline_ns; /* uv_hrtime() snapshot taken at goc_timeout() call time */
} goc_timeout_req;

goc_chan* goc_timeout(uint64_t ms) {
    goc_chan* ch = goc_chan_make(0);
    goc_timeout_req* req = malloc(sizeof(goc_timeout_req));
    uv_timer_t* t = malloc(sizeof(uv_timer_t));
    t->data = ch;
    req->ch = ch; req->t = t; req->ms = ms;
    req->deadline_ns = uv_hrtime();              /* record wall-clock call time */
    uv_async_init(g_loop, &req->async, on_start_timer);  /* safe from any thread */
    uv_async_send(&req->async);                           /* dispatch to loop thread */
    return ch;
}

/* runs on the loop thread */
static void on_start_timer(uv_async_t* h) {
    goc_timeout_req* req = (goc_timeout_req*)h;
    /* Subtract async dispatch latency so the timer fires at the wall-clock
     * deadline recorded above, not req->ms after this callback happens to run.
     * Clamp to zero if the deadline has already passed — fire next iteration. */
    uint64_t now_ns     = uv_hrtime();
    uint64_t elapsed_ms = (now_ns > req->deadline_ns)
                        ? (now_ns - req->deadline_ns) / 1000000ULL : 0;
    uint64_t remaining  = (elapsed_ms < req->ms) ? (req->ms - elapsed_ms) : 0;
    uv_timer_init(g_loop, req->t);
    uv_timer_start(req->t, on_timeout, remaining, 0);
    uv_close((uv_handle_t*)h, free_start_cb);  /* one-shot: close after use */
}

static void on_timeout(uv_timer_t* t) {
    goc_chan* ch = (goc_chan*)t->data;
    goc_close(ch);                     /* wake any parked takers with ok==GOC_CLOSED */
    uv_close((uv_handle_t*)t, free_timer_cb);
    /* goc_close is safe to call concurrently with goc_shutdown's Step 1 close
       loop because it is serialised by a close_guard CAS: exactly one caller
       flips close_guard from 0→1 and proceeds with teardown; the other sees 1
       and returns immediately — no double-free, no segfault. */
}

static void free_start_cb(uv_handle_t* h) {
    free(h);   /* h == &req->async == req (first member); frees the whole req.
                  Valid by C11 §6.7.2.1p15: a pointer to a struct may be converted
                  to a pointer to its first member and vice versa. uv_async_t is
                  itself a uv_handle_t via UV_HANDLE_FIELDS, making the full chain
                  uv_handle_t* → uv_async_t* → goc_timeout_req* well-defined. */
}

static void free_timer_cb(uv_handle_t* h) {
    free(h);   /* safe: uv_close guarantees no further callbacks */
}
```

> **Dispatch-latency correction.** `uv_timer_start` is called from `on_start_timer`, which runs on the loop thread some time *after* `goc_timeout()` returned to the caller. Without correction, the observable latency from `goc_timeout()` to channel-close would be `async_dispatch_delay + ms`, not `ms`. On a loaded system the dispatch delay can be hundreds of milliseconds — large enough to exceed any reasonable upper-bound assertion in tests. The fix is to snapshot `uv_hrtime()` at call time, compute how many milliseconds of the budget were consumed by the dispatch, and pass the reduced `remaining` duration to `uv_timer_start`. If the budget is already exhausted (`remaining == 0`), libuv fires the timer on the very next loop iteration, which is the correct expired-deadline behaviour.

Runs entirely on the uv loop thread (after the async dispatch). Composes with `goc_alts` for deadline semantics:

```c
goc_alt_op ops[] = {
    { .ch = data_ch,          .op_kind = GOC_ALT_TAKE },
    { .ch = goc_timeout(500), .op_kind = GOC_ALT_TAKE },
};
goc_alts_result r = goc_alts(ops, 2);
```

---

## `goc_alts`

```c
typedef enum {
    GOC_ALT_TAKE,    /* receive from ch */
    GOC_ALT_PUT,     /* send put_val into ch */
    GOC_ALT_DEFAULT, /* fires immediately if no other arm is ready; ch must be NULL */
} goc_alt_kind;

typedef struct { goc_chan* ch; goc_alt_kind op_kind; void* put_val; } goc_alt_op;
typedef struct { size_t index; goc_val_t value; } goc_alts_result;

goc_alts_result goc_alts     (goc_alt_op* ops, size_t n); /* fiber context */
goc_alts_result goc_alts_sync(goc_alt_op* ops, size_t n); /* blocking OS thread */
```

`goc_alts` may suspend the calling fiber and must only be called from within a fiber. `goc_alts_sync` blocks the calling OS thread and must not be called from within a fiber.

The returned `goc_alts_result.index` is the zero-based index of the winning arm. `goc_alts_result.value` is a `goc_val_t`. For take arms, `value.ok == GOC_CLOSED` means the channel was closed rather than that a `NULL` was sent. For put arms, the winning result is always `{NULL, GOC_OK}` — `result_slot` is NULL for put entries and `wake()` skips the `result_slot` write. For a `GOC_ALT_DEFAULT` arm, the winning result is `{NULL, GOC_OK}`.

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

   **Park:** For `goc_alts`: call `mco_yield` to suspend the fiber. For `goc_alts_sync`: block on a semaphore. Copy the fiber/semaphore context into every `entries[i]`. Set `entries[i]->arm_idx = i`.

   **`goc_alts_sync` semaphore teardown:** After `sem_wait` returns, call `sem_destroy(&shared_sem)` before returning. This is safe: `wake` wins the `woken` CAS and completes `sem_post` before any other path can call `sem_post` on the same semaphore. All other entries either have their `cancelled` flag set or lose the `woken` CAS, so no additional `sem_post` on `shared_sem` will occur after `sem_wait` unblocks. `sem_destroy` may therefore be called immediately.

6. **On wake (park path only):** The first channel to fire performs a CAS on `fired` (0→1, `acq_rel`). The storing thread writes the delivered value into `result_slot` **before** the CAS; the resuming fiber reads `fired` with `memory_order_acquire`, guaranteeing it observes the written value. The resuming fiber then iterates all entries and writes `cancelled = 1` on every entry where `woken == 0` (the losers).

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
                 GOC_ALT_DEFAULT                       } goc_alt_kind;
typedef struct { goc_chan* ch; goc_alt_kind op_kind;
                 void* put_val;                        } goc_alt_op;
typedef struct { size_t index; goc_val_t value;        } goc_alts_result;
typedef enum {
    GOC_DRAIN_OK      = 0,  /* all fibers finished within the deadline */
    GOC_DRAIN_TIMEOUT = 1,  /* deadline expired; pool remains valid and running */
} goc_drain_result_t;

/* Init / shutdown */
void          goc_init(void);
void          goc_shutdown(void);

/* Memory */
void*         goc_malloc(size_t n);

/* Channels */
goc_chan*     goc_chan_make(size_t buf_size);
void          goc_close(goc_chan* ch);

/* Utilities */
bool          goc_in_fiber(void);   /* true if caller is inside a fiber, false otherwise */

/* Fiber launch — both return a join channel closed when the fiber returns */
goc_chan*     goc_go(void (*fn)(void*), void* arg);
goc_chan*     goc_go_on(goc_pool* pool, void (*fn)(void*), void* arg);

/* Channel I/O — fiber context */
goc_val_t     goc_take(goc_chan* ch);
goc_status_t  goc_put(goc_chan* ch, void* val);

/* Channel I/O — non-blocking (any context) */
goc_val_t     goc_take_try(goc_chan* ch);   /* ok==GOC_EMPTY if open but empty */

/* Channel I/O — blocking OS thread */
goc_val_t     goc_take_sync(goc_chan* ch);
goc_status_t  goc_put_sync(goc_chan* ch, void* val);

/* Channel I/O — callbacks (any context; cb invoked on uv loop thread) */
void          goc_take_cb(goc_chan* ch,
                          void (*cb)(void* val, goc_status_t ok, void* ud), void* ud);
void          goc_put_cb(goc_chan* ch, void* val,
                         void (*cb)(goc_status_t ok, void* ud), void* ud);

/* Select */
goc_alts_result goc_alts     (goc_alt_op* ops, size_t n);
goc_alts_result goc_alts_sync(goc_alt_op* ops, size_t n);

/* Timeout */
goc_chan*     goc_timeout(uint64_t ms);

/* Thread pool */
goc_pool*          goc_pool_make(size_t threads);
void               goc_pool_destroy(goc_pool* pool);         /* blocking drain-and-join */
goc_drain_result_t goc_pool_destroy_timeout(goc_pool* pool,
                                            uint64_t ms);    /* GOC_DRAIN_OK or GOC_DRAIN_TIMEOUT; pool remains valid on timeout */

/* Scheduler / event loop */
uv_loop_t*    goc_scheduler(void);
```

### Internal Functions

| Function | Signature | Defined in |
|---|---|---|
| `cb_queue_init` | `void cb_queue_init(void)` | `loop.c` |
| `post_to_run_queue` | `void post_to_run_queue(goc_pool* pool, goc_entry* entry)` | `pool.c` — increments `active_count` only; does **not** touch `live_count` |
| `pool_fiber_born` | `void pool_fiber_born(goc_pool* pool)` | `pool.c` — increments `live_count` exactly once per new fiber; called from `goc_go_on` before `post_to_run_queue` |
| `post_callback` | `void post_callback(goc_entry* entry, void* value)` | `loop.c` |
| `on_wakeup` | `void on_wakeup(uv_async_t* handle)` | `loop.c` |
| `wake` | `void wake(goc_chan* ch, goc_entry* entry, void* value)` | `channel.c` |
| `compact_dead_entries` | `void compact_dead_entries(goc_chan* ch)` | `channel.c` |
| `goc_chan_destroy` | `void goc_chan_destroy(goc_chan* ch)` | `channel.c` — stub only; mutex teardown is deferred to `goc_shutdown` Step 2 |
| `chan_register` | `void chan_register(goc_chan* ch)` | `gc.c` — appends `ch` to the `live_channels` array under `g_live_mutex`; called by `goc_chan_make`. This list is the sole input to the graceful-drain wait in `goc_shutdown`. |
| `chan_unregister` | `void chan_unregister(goc_chan* ch)` | `gc.c` — removes `ch` from `live_channels` under `g_live_mutex`; called by `goc_close`. |
| `chan_take_from_buffer` | `static inline int chan_take_from_buffer(goc_chan*, void**)` | `internal.h` — requires `struct goc_chan` in scope; pulled in via `chan_type.h` |
| `chan_take_from_putter` | `static inline int chan_take_from_putter(goc_chan*, void**)` | `internal.h` — requires `struct goc_chan` in scope; pulled in via `chan_type.h`; snapshots `e->next` before `wake()` to avoid UAF on stack-allocated entries |
| `chan_put_to_taker` | `static inline int chan_put_to_taker(goc_chan*, void*)` | `internal.h` — requires `struct goc_chan` in scope; pulled in via `chan_type.h`; snapshots `e->next` before `wake()` to avoid UAF on stack-allocated entries |
| `chan_put_to_buffer` | `static inline int chan_put_to_buffer(goc_chan*, void*)` | `internal.h` — requires `struct goc_chan` in scope; pulled in via `chan_type.h` |

> **Note:** `chan_register` and `chan_unregister` are *defined* in `gc.c` but *called* from `channel.c` (`goc_chan_make` calls `chan_register`; `goc_close` calls `chan_unregister`). The inline ring-buffer helpers in `internal.h` depend on the full `struct goc_chan` layout, which is provided by `chan_type.h`. `internal.h` includes `chan_type.h` directly, so any `.c` file that includes `internal.h` automatically has the full struct in scope.

### `goc_scheduler`

Returns the `uv_loop_t*` owned by the runtime. Must be called only after `goc_init` has returned. Once `goc_init` has returned the pointer is stable and read-only; it is safe to call from any thread at any point after that without synchronisation.

Primary use-case: registering user-owned libuv handles so they share the same event loop as the rest of the runtime.

```c
uv_tcp_t* server = malloc(sizeof(uv_tcp_t));   /* malloc — NOT GC heap */
uv_tcp_init(goc_scheduler(), server);
```

> **Do not call `uv_run` or `uv_loop_close` on the returned pointer.** The loop lifetime is managed entirely by `libgoc`.

---

## Initialization Sequence

`goc_init` must be called exactly once, as the first call in `main()`. Calling it more than once is undefined behaviour. It performs the following steps:

1. **`gc.c`** — Call `GC_INIT()` then `GC_allow_register_threads()` unconditionally. `GC_INIT()` performs all one-time GC setup; `GC_allow_register_threads()` enables multi-thread stack registration and must follow it. This must happen before any `goc_malloc` call.
2. **`gc.c`** — Initialise the `live_channels` list: a `malloc`-allocated, dynamically grown `goc_chan**` array protected by a plain `pthread_mutex_t`. Must be fully initialised before the loop thread is spawned (step 4) — the loop thread could indirectly trigger `goc_chan_make`, which acquires `g_live_mutex`. `goc_chan_make` is responsible for appending to it; `goc_close` removes entries. Required by `goc_shutdown`.
3. **`pool.c`** — Initialise the global pool registry (a `malloc`-allocated list protected by a `pthread_mutex_t`). Must be fully initialised before the loop thread is spawned (step 4). Read `GOC_POOL_THREADS` from the environment; default to `max(4, hardware_concurrency)`. Every subsequent `goc_pool_make` call appends to this registry; `goc_pool_destroy` removes the entry. `goc_shutdown` iterates the registry to drain and destroy any pools that remain at teardown time.
4. **`loop.c`** — Call `loop_init()`. Internally this: allocates and initialises `g_loop`; malloc-allocates and initialises both `uv_async_t` handles (`g_wakeup` via atomic release store, `g_shutdown_async`); calls `cb_queue_init()` to allocate the MPSC sentinel node (must precede thread spawn and requires GC already up); then spawns the dedicated loop thread via plain `pthread_create`. All shared state the loop thread can reach (`g_live_mutex`, pool registry, async handles, callback queue) is fully initialised at this point. Because the loop thread is created with `pthread_create` rather than `GC_pthread_create`, it is outside the GC's automatic registration wrapper — it calls `GC_get_stack_base` and `GC_register_my_thread` at entry and `GC_unregister_my_thread` before exit to ensure its stack is scanned during stop-the-world collection. It runs `while (uv_run(g_loop, UV_RUN_ONCE)) {}` for its entire lifetime.
5. **`pool.c`** — Call `goc_pool_make(N)` for the default pool, storing the result globally. This spawns the worker threads; done last so workers start only after the full runtime is up.

---

## Shutdown Sequence

`goc_shutdown` performs orderly teardown in five steps. It must be called from outside any fiber or pool thread (e.g. the application's main thread). It must not be called concurrently with any other `libgoc` function. It will block until all fibers have completed naturally — if any fiber is waiting on a channel event that will never arrive, `goc_shutdown` will hang. After it returns, the runtime is fully torn down; `goc_init` may not be called again.

> **Post-shutdown API boundary:** Once `goc_shutdown` returns, calling any `libgoc` function is **undefined behaviour**. Step 2 destroys and frees all channel mutexes; any subsequent call that reaches `uv_mutex_lock(ch->lock)` — including `goc_close`, `goc_take_try`, `goc_chan_make`, or any other channel API — accesses freed memory. Do not retain `goc_chan*` pointers across `goc_shutdown` with the intent to use them after the call returns.

**Step 1 — Drain all in-flight fibers.**

`goc_shutdown` iterates the global pool registry and calls `goc_pool_destroy` on every pool that has not already been destroyed — this includes the default pool and any user-created pools made with `goc_pool_make`. `goc_pool_destroy` waits on `drain_cond` (under `drain_mutex`) until `live_count` reaches zero — meaning every fiber has run to completion. Fibers are expected to finish naturally; `goc_shutdown` does not forcibly close channels or inject `ok==GOC_CLOSED` wakeups.

Once the drain-wait completes, `goc_pool_destroy`:
1. Logs a diagnostic if the run queue is somehow still non-empty.
2. Sets `pool->shutdown = 1` (atomic release store).
3. Posts one semaphore unit per worker to unblock sleeping threads.
4. Joins every worker thread.
5. Destroys the semaphore and drain primitives, drains and frees the run queue, and frees the pool.

Because all fibers have returned before `goc_pool_destroy` returns, no code path will ever call `uv_mutex_lock` on a channel lock again. Channel mutexes may therefore be destroyed immediately after this point without deferral.

**Step 2 — Destroy channel mutexes.**

After `goc_pool_destroy` returns, iterate the `live_channels` list, calling `uv_mutex_destroy` and `free` on each channel's `lock` pointer, then free the list itself.

**Step 3 — Signal the loop thread to close both `uv_async_t` handles.**

With the pool fully destroyed, no fiber or pool thread will ever call `post_callback` again. `goc_shutdown` calls `uv_async_send(g_shutdown_async)` from the main thread. `uv_async_send` is the only libuv call documented as safe to call from any thread.

The loop-thread callback `on_shutdown_signal` first drains `g_cb_queue` completely before calling `uv_close` on either handle. Although the pool is gone by this point, any entries pushed to `g_cb_queue` before the pool was destroyed and not yet flushed by a prior `on_wakeup` invocation are drained here. After draining, `on_shutdown_signal` calls `uv_close` on `g_wakeup` (with `on_wakeup_closed`) and on `g_shutdown_async` itself (with `on_shutdown_async_closed`). Both closes are performed from the loop thread, which is the only correct context: `uv_close` is not thread-safe and races against the loop's internal handle-list traversal if called from another thread.

Once both close callbacks have fired, the loop has no remaining active handles and `loop_thread_fn` exits. The loop thread runs `while (uv_run(g_loop, UV_RUN_ONCE)) {}` for its entire lifetime — both during normal operation and during shutdown drain. `UV_RUN_ONCE` is used rather than `UV_RUN_DEFAULT` because `UV_RUN_DEFAULT` returns as soon as there are no pending callbacks in one iteration, which may be before all `uv_close` callbacks have completed; the `UV_RUN_ONCE` spin correctly drains all remaining callbacks before returning zero.

> **Why not call `uv_run` from the main thread?** libuv explicitly forbids running the same loop from two threads simultaneously — doing so causes data races on the loop's internal timer heap, handle lists, and pending queue. The correct approach is to let the loop thread's own `uv_run` drain naturally (which happens once all handles are closed), then join the thread.

**Step 4 — Join the loop thread.**

`pthread_join(g_loop_thread)` blocks until `loop_thread_fn` exits. After this point:

- All `on_wakeup` and `on_shutdown_signal` deliveries are guaranteed to have completed.
- No libuv callback will ever fire again.
- `g_loop` is safe to close and free.

**Step 5 — Close and free the uv loop.**

`uv_loop_close(g_loop)` is called and its return value is asserted to be `0`. Because the loop thread has already exited (Step 4) and all handles were closed inside Step 3, `uv_loop_close` must succeed; a non-zero return indicates a handle was left open and is treated as a programming error. The loop is then freed and `g_loop` set to `NULL`.

---

## Testing

The test suite is split across phase files in `tests/`, each a self-contained C file with no external test framework dependency.

### Design

- **Harness**: `tests/test_harness.h` provides shared `TEST_BEGIN` / `ASSERT` / `TEST_PASS` / `TEST_FAIL` macros with `goto done` cleanup — no `setjmp`. All phase test files include this header instead of duplicating the macros.
- **Crash handler**: `test_harness.h` also provides `install_crash_handler()`, which registers a `SIGSEGV` and `SIGABRT` signal handler. The handler calls `backtrace_symbols_fd()` to print a full backtrace to `stderr`, then re-raises the signal with the default disposition restored so the process exits with the correct signal status. `install_crash_handler()` is called as the first statement of `main()` in each phase binary, before `goc_init()`. This approach is used in preference to core dumps because GitHub Actions runners do not reliably write core files to disk (apport and systemd-coredump intercept them at the kernel level), whereas `stderr` output is always captured by CTest `--output-on-failure`. Test executables are linked with `-rdynamic` so that `backtrace_symbols_fd()` can resolve function names; without it, frames appear as raw addresses only.
- **Isolation**: `goc_init()` and `goc_shutdown()` bracket the entire test binary — called once each in the `main()` of **each phase's test binary** before and after all tests in that phase run. Individual test functions do not call them.
- **Synchronisation**: a `done_t` helper (a plain POSIX `sem_t` — `done_signal` calls `sem_post`, `done_wait` calls `sem_wait`) lets the main thread block until fibers finish without relying on `sleep` or a `goc_chan`.
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

**Phase 6 — Thread pool**

| Test | Description |
|---|---|
| P6.1 | `goc_pool_make` / `goc_pool_destroy` lifecycle; `goc_go_on` dispatches fiber to correct pool |
| P6.2 | `goc_pool_destroy_timeout` returns `GOC_DRAIN_OK` when all fibers finish before the deadline |
| P6.3 | `goc_pool_destroy_timeout` returns `GOC_DRAIN_TIMEOUT` when fibers are still running at deadline; pool remains valid — verified by dispatching a new short-lived fiber via `goc_go_on` to the same pool and confirming it runs to completion before `goc_pool_destroy` is called |
| P6.4 | `goc_malloc` end-to-end: fiber builds GC-heap linked list, main traverses after join |
| P6.5 | `goc_alts` with `n > GOC_ALTS_STACK_THRESHOLD` (8) arms exercises the `malloc` path in `alts_dedup_sort_channels`; correct arm fires, no memory error (run under ASAN to catch heap misuse) |

> **Not yet tested:** `compact_dead_entries` sweep — verifying that the amortised dead-entry sweep fires correctly when `dead_count >= GOC_DEAD_COUNT_THRESHOLD` after many fibers race on the same channel via `goc_alts`. The sweep logic exists and is exercised indirectly by the integration tests, but a dedicated test that asserts the sweep threshold, entry unlinking, and channel consistency after the sweep has not yet been written.

**Phase 7 — Integration**

| Test | Description |
|---|---|
| P7.1 | Pipeline: producer → transformer → consumer, 16 items, all values correct |
| P7.2 | Fan-out / fan-in: 1 producer, 4 workers, result aggregation, 20 items, sum verified |
| P7.3 | High-volume stress: 10 000 messages, sum verified |
| P7.4 | Multi-fiber: 8 senders on 1 unbuffered channel, all IDs received exactly once |
| P7.5 | Timeout + cancellation: slower fiber's result discarded cleanly, shutdown completes without hang |

**Phase 8 — Safety and crash behaviour**

| Test | Description |
|---|---|
| P8.1 | Stack overflow: an `overflow_fiber` corrupts its own canary word then parks on `goc_take`; a `sender_fiber` calls `goc_put` to wake it; `pool_worker_fn` checks the canary before the next `mco_resume`, finds it corrupted, and calls `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT`. Two fibers are used (rather than a fiber + `goc_take_sync` from the OS thread) to guarantee the victim parks before the sender runs — with `goc_take_sync`, a race exists where the OS thread parks first and the fiber's `goc_put` rendezvouses immediately without ever suspending, so the canary check never fires. |
| P8.2 | `goc_take` called from a bare OS thread (not a fiber) → `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT` |
| P8.3 | `goc_put` called from a bare OS thread (not a fiber) → `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT` |
| P8.4 | `goc_alts` called with more than one `GOC_ALT_DEFAULT` arm (from within a fiber) → `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT`. A fiber is spawned via `goc_go()` to provide fiber context. |
| P8.5 | `goc_alts_sync` called with more than one `GOC_ALT_DEFAULT` arm → `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT` |
| P8.6 | `goc_pool_destroy` called from within the target pool's own worker thread → `abort()`; verified via `fork` + `waitpid` asserting `SIGABRT` |

### Running

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
# or run a single phase directly:
./build/test_p1_foundation
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
