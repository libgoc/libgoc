/*
 * tests/test_p8_safety.c — Phase 8: Safety and crash behaviour tests for libgoc
 *
 * Verifies that the runtime detects and terminates on undefined or unsafe
 * usage patterns before they can cause silent corruption.  Every test in this
 * phase expects the child process to exit with SIGABRT; each test is run in a
 * forked child so that an abort in the child never terminates the parent test
 * harness.
 *
 * Build:  cmake -B build && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_p8_safety
 *
 * Compile requirements: -std=c11 -DGC_THREADS
 *
 * Dependencies:
 *   - libgoc (goc.h)  — runtime under test
 *   - Boehm GC        — must be the threaded variant (bdw-gc-threaded);
 *                        initialised internally by goc_init()
 *   - libuv           — event loop; drives fiber scheduling
 *   - POSIX fork / waitpid — used to isolate each abort()-inducing test in a
 *                        child process; the parent waits for the child and
 *                        verifies it was killed by SIGABRT.
 *                        Not available on Windows — all P8 tests are skipped
 *                        on that platform.
 *
 * Test isolation via fork (Linux / macOS only):
 *   Each test that is expected to call abort() spawns a child with fork().
 *   The child runs goc_init(), performs the unsafe operation, and should never
 *   return — the runtime calls abort() before that is possible.  The parent
 *   waits with waitpid() and checks WIFSIGNALED(status) && WTERMSIG(status)
 *   == SIGABRT.  If the child exits normally (no signal) the test fails.
 *
 *   The crash handler installed by install_crash_handler() is intentionally
 *   NOT called in the child: the child must exit via the runtime's own abort()
 *   call, not via a SIGSEGV handler.  The parent's crash handler is installed
 *   before fork() so that any unexpected crash in the parent itself is still
 *   reported with a backtrace.
 *
 *   Note: goc_init() must be called in every child process because goc state
 *   is process-local.  The child never calls goc_shutdown() — it is expected
 *   to abort() before reaching that point.
 *
 * Test coverage (Phase 8 — Safety and crash behaviour):
 *
 *   P8.1   Stack overflow: a fiber that exhausts its 64 KB stack overwrites
 *          the canary → pool worker calls abort() before the next mco_resume;
 *          verified via fork + waitpid asserting SIGABRT
 *   P8.2   goc_take() called from a bare OS thread (not a fiber) → abort();
 *          verified via fork + waitpid asserting SIGABRT
 *   P8.3   goc_put() called from a bare OS thread (not a fiber) → abort();
 *          verified via fork + waitpid asserting SIGABRT
 *   P8.4   goc_alts() with multiple default arms → abort();
 *          verified via fork + waitpid asserting SIGABRT
 *   P8.5   goc_alts_sync() with multiple default arms → abort();
 *          verified via fork + waitpid asserting SIGABRT
 *   P8.6   goc_pool_destroy() called from within the target pool's own
 *          worker thread → abort(); verified via fork + waitpid asserting
 *          SIGABRT
 *   P8.7   goc_init() called from a non-main pthread → abort(); verified
 *          via fork + waitpid asserting SIGABRT
 *   P8.8   goc_shutdown() called from a non-main pthread → abort(); verified
 *          via fork + waitpid asserting SIGABRT
 *   P8.9   goc_take_sync() called from a fiber → abort(); verified via
 *          fork + waitpid asserting SIGABRT
 *   P8.10  goc_put_sync() called from a fiber → abort(); verified via
 *          fork + waitpid asserting SIGABRT
 *   P8.11  goc_alts_sync() called from a fiber → abort(); verified via
 *          fork + waitpid asserting SIGABRT
 *
 * Notes:
 *   - goc_init() is called once in the parent main() before forking, but
 *     each child also calls goc_init() independently because the forked
 *     address space inherits the parent's (partially-initialised) GC state
 *     in an inconsistent way.  Each child must re-initialise from scratch.
 *   - goc_shutdown() is called once in the parent main() after all tests
 *     complete.
 *   - The goto-based cleanup pattern from the harness (ASSERT → done:) is
 *     used in the parent-side test wrappers only; child-side code has no
 *     cleanup label.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>

#if !defined(_WIN32)
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#include "test_harness.h"
#include "goc.h"
#include "minicoro.h"

#if !defined(_WIN32)

/* =========================================================================
 * fork_expect_sigabrt — helper that forks and asserts the child dies with
 *                        SIGABRT.
 *
 * Forks a child process.  The child calls child_fn(arg) and should never
 * return — the runtime is expected to call abort() from within child_fn.
 *
 * The parent blocks in waitpid().  Returns true if the child was killed by
 * SIGABRT, false otherwise (e.g. exited normally or died with another signal).
 * ====================================================================== */

typedef void (*child_fn_t)(void* arg);

static bool fork_expect_sigabrt(child_fn_t child_fn, void* arg) {
    pid_t pid = fork();
    if (pid < 0) {
        /* fork failed — treat as a test failure */
        return false;
    }

    if (pid == 0) {
        /*
         * Child process.
         *
         * Re-initialise the runtime from scratch.  The forked address space
         * inherits the parent's memory image but libuv handles, mutexes, and
         * the GC's internal thread table are all in an inconsistent state
         * because the background threads were not forked.  A fresh goc_init()
         * in the child is required before any goc_* call.
         *
         * Reset SIGABRT to SIG_DFL.  The parent installed a crash handler
         * (install_crash_handler) that catches SIGABRT, prints a backtrace,
         * and re-raises with raise().  The child inherits that handler via
         * fork().  When abort() fires on a pool worker thread it blocks
         * SIGABRT internally before raising it; the inherited crash handler's
         * subsequent raise(SIGABRT) then interacts with that signal mask and
         * can cause the process to hang instead of terminating.  Restoring
         * SIG_DFL ensures abort() kills the child immediately so waitpid
         * sees WIFSIGNALED(...SIGABRT).
         */
        struct sigaction sa_dfl = { .sa_handler = SIG_DFL };
        sigemptyset(&sa_dfl.sa_mask);
        sigaction(SIGABRT, &sa_dfl, NULL);

        /* Force a single pool worker so fibers execute sequentially.
         * P8.1 relies on overflow_fiber parking before sender_fiber runs;
         * with more than one worker both could be dequeued simultaneously
         * and the ordering would not be guaranteed. */
        setenv("GOC_POOL_THREADS", "1", 1);

        goc_init();
        child_fn(arg);
        /* Should never reach here — if we do, exit with a distinctive code
         * (2) so the parent can tell the child completed without aborting. */
        _exit(2);
    }

    /* Parent: wait for the child and inspect its exit status. */
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        return false;
    }

    return WIFSIGNALED(status) && (WTERMSIG(status) == SIGABRT);
}

/* =========================================================================
 * Phase 8 — Safety and crash behaviour
 * ====================================================================== */

/* --- P8.1: Stack overflow overwrites canary → abort() ------------------- */

/*
 * overflow_fiber — fiber entry point for P8.1 (the "victim").
 *
 * Corrupts its own stack canary then parks on goc_take(ch), waiting for
 * a rendezvous partner.  When fiber_b sends on ch, pool_worker_fn re-queues
 * this fiber and checks the canary before the next mco_resume — finding it
 * corrupted — and calls abort().
 *
 * Using a fiber-to-fiber rendezvous (rather than goc_take_sync from the OS
 * thread) guarantees the victim always parks before the sender wakes it.
 * goc_take_sync races: if the OS thread calls goc_take_sync before the fiber
 * has run, the fiber's goc_put finds a waiting taker and completes without
 * suspending — so the canary check never fires.  Two fibers on the same pool
 * avoid this race because the scheduler runs them sequentially.
 */
static void overflow_fiber(void* c) {
    goc_chan* ch = (goc_chan*)c;

    /* Corrupt the canary at the base of this fiber's stack. */
    mco_coro* self = mco_running();
    volatile uint32_t* canary = (volatile uint32_t*)self->stack_base;
    *canary = 0u;

    /*
     * Park on goc_take.  The pool worker suspends this fiber and moves on.
     * When the sender fiber calls goc_put(ch), wake() re-queues this fiber.
     * On the next iteration the pool worker checks the canary, finds it
     * corrupted, and calls abort().
     */
    goc_take(ch);
    /* Unreachable: abort() fires before mco_resume returns. */
}

/*
 * sender_fiber — sends one value on ch to wake overflow_fiber.
 *
 * Spawned after overflow_fiber so the victim is guaranteed to be parked
 * (MCO_SUSPENDED) by the time the sender runs on the same pool.
 */
static void sender_fiber(void* c) {
    goc_chan* ch = (goc_chan*)c;
    goc_put(ch, (void*)1);
    /* Unreachable if abort() fires before pool_worker_fn resumes overflow_fiber. */
}

static void p8_1_child_fn(void* arg) {
    (void)arg;

    /*
     * Unbuffered channel: overflow_fiber parks on goc_take; sender_fiber
     * calls goc_put to wake it.  Because both fibers run on the same pool
     * (single worker if GOC_POOL_THREADS=1, but the ordering still holds),
     * overflow_fiber is always suspended before sender_fiber executes.
     *
     * After wake(), pool_worker_fn re-queues overflow_fiber and on the next
     * iteration checks the canary → abort() → SIGABRT kills the child.
     *
     * The main thread blocks in pause(); abort() from the pool worker sends
     * SIGABRT to the whole process, which with SIG_DFL terminates it.
     */
    goc_chan* ch = goc_chan_make(0);
    goc_go(overflow_fiber, ch);
    goc_go(sender_fiber,   ch);

    /* Block the main thread; abort() from the pool worker kills the process. */
    pause(); /* blocked until a signal — abort() terminates the whole process */
    /* Unreachable. */
}

/*
 * P8.1 — Stack overflow: canary overwrite detected, runtime calls abort()
 *
 * Forks a child that:
 *   1. Spawns a fiber that directly corrupts its canary word (simulating the
 *      effect of a stack overflow reaching the bottom of the stack region).
 *   2. The fiber yields to the pool worker via goc_put.
 *   3. The pool worker checks the canary before the next mco_resume, finds it
 *      corrupted, and calls abort().
 *
 * The parent verifies the child was killed by SIGABRT.
 */
static void test_p8_1(void) {
    TEST_BEGIN("P8.1   stack overflow: canary overwrite → abort()");

#ifdef LIBGOC_VMEM_ENABLED
    /* In virtual memory mode the canary macros are no-ops — the canary check
     * in pool_worker_fn never fires, so the child would never call abort().
     * Skip here to avoid an infinite wait in fork_expect_sigabrt. */
    printf("SKIPPED (virtual memory mode)\n");
    return;
#else
    bool got_sigabrt = fork_expect_sigabrt(p8_1_child_fn, NULL);
    ASSERT(got_sigabrt);
    TEST_PASS();
done:;
#endif
}

/* --- P8.2: goc_take() from a bare OS thread → abort() ------------------- */

/*
 * Child function for P8.2.
 *
 * Calls goc_take() directly from the main (OS) thread, which is not a fiber.
 * goc_take() checks goc_in_fiber() and abort()s if the caller is not in a
 * fiber context.
 *
 * A dummy rendezvous channel is created so that goc_take() has a valid target;
 * the check fires before any channel operation takes place.
 */
static void p8_2_child_fn(void* arg) {
    (void)arg;
    goc_chan* ch = goc_chan_make(0);
    /* Call goc_take from a bare OS thread — must abort(). */
    goc_take(ch);
    /* Unreachable. */
}

/*
 * P8.2 — goc_take() from a bare OS thread → abort()
 *
 * Verifies that calling goc_take() outside a fiber causes the runtime to
 * abort() immediately.  Uses fork + waitpid to isolate the expected crash.
 */
static void test_p8_2(void) {
    TEST_BEGIN("P8.2   goc_take() from OS thread → abort()");
    bool got_sigabrt = fork_expect_sigabrt(p8_2_child_fn, NULL);
    ASSERT(got_sigabrt);
    TEST_PASS();
done:;
}

/* --- P8.3: goc_put() from a bare OS thread → abort() -------------------- */

/*
 * Child function for P8.3.
 *
 * Calls goc_put() directly from the main (OS) thread.  As with goc_take(),
 * the fiber-context assertion fires and the runtime calls abort().
 *
 * A buffered channel (capacity 1) is used so the put would ordinarily succeed
 * without a rendezvous partner; the abort must fire before the channel is
 * touched.
 */
static void p8_3_child_fn(void* arg) {
    (void)arg;
    goc_chan* ch = goc_chan_make(1);
    /* Call goc_put from a bare OS thread — must abort(). */
    goc_put(ch, goc_box_uint(0xCAFE));
    /* Unreachable. */
}

/*
 * P8.3 — goc_put() from a bare OS thread → abort()
 *
 * Verifies that calling goc_put() outside a fiber causes the runtime to
 * abort() immediately.  Uses fork + waitpid to isolate the expected crash.
 */
static void test_p8_3(void) {
    TEST_BEGIN("P8.3   goc_put() from OS thread → abort()");
    bool got_sigabrt = fork_expect_sigabrt(p8_3_child_fn, NULL);
    ASSERT(got_sigabrt);
    TEST_PASS();
done:;
}

/* --- P8.4: goc_alts() with multiple default arms → abort() --------------- */

/*
 * p8_4_alts_fiber — fiber entry point for P8.4.
 * Calls goc_alts() with 2 default arms, which should trigger an abort()
 * in Phase 1 validation.
 */
static void p8_4_alts_fiber(void* arg) {
    (void)arg;

    goc_alt_op_t ops[2] = {
        { .op_kind = GOC_ALT_DEFAULT, .ch = NULL },
        { .op_kind = GOC_ALT_DEFAULT, .ch = NULL }
    };

    /* Call goc_alts with 2 defaults — must abort() in Phase 1. */
    goc_alts(ops, 2);
    /* Unreachable. */
}

static void p8_4_child_fn(void* arg) {
    (void)arg;

    /* Spawn a fiber that calls goc_alts with multiple defaults. */
    goc_go(p8_4_alts_fiber, NULL);

    /* Block the main thread; abort() from the fiber kills the process. */
    pause(); /* blocked until a signal — abort() terminates the whole process */
    /* Unreachable. */
}

/*
 * P8.4 — goc_alts() with multiple default arms → abort()
 *
 * Verifies that goc_alts() detects and rejects the invariant violation
 * of having more than one default arm.  Uses fork + waitpid to isolate the
 * expected crash.
 */
static void test_p8_4(void) {
    TEST_BEGIN("P8.4   goc_alts() with multiple defaults → abort()");
    bool got_sigabrt = fork_expect_sigabrt(p8_4_child_fn, NULL);
    ASSERT(got_sigabrt);
    TEST_PASS();
done:;
}

/* --- P8.5: goc_alts_sync() with multiple default arms → abort() --------- */

static void p8_5_child_fn(void* arg) {
    (void)arg;

    goc_alt_op_t ops[2] = {
        { .op_kind = GOC_ALT_DEFAULT, .ch = NULL },
        { .op_kind = GOC_ALT_DEFAULT, .ch = NULL }
    };

    /* Call goc_alts_sync with 2 defaults — must abort(). */
    goc_alts_sync(ops, 2);
    /* Unreachable. */
}

/*
 * P8.5 — goc_alts_sync() with multiple default arms → abort()
 *
 * Verifies that goc_alts_sync() detects and rejects the invariant violation
 * of having more than one default arm.  Uses fork + waitpid to isolate the
 * expected crash.
 */
static void test_p8_5(void) {
    TEST_BEGIN("P8.5   goc_alts_sync() with multiple defaults → abort()");
    bool got_sigabrt = fork_expect_sigabrt(p8_5_child_fn, NULL);
    ASSERT(got_sigabrt);
    TEST_PASS();
done:;
}

/* --- P8.6: goc_pool_destroy() from pool worker thread → abort() -------- */

static void p8_6_destroy_self_fiber(void* arg) {
    goc_pool* pool = (goc_pool*)arg;

    /* Must abort(): self-destroy from a worker thread is invalid. */
    goc_pool_destroy(pool);
    /* Unreachable. */
}

static void p8_6_child_fn(void* arg) {
    (void)arg;

    goc_pool* pool = goc_pool_make(1);
    goc_go_on(pool, p8_6_destroy_self_fiber, pool);

    /* Block main thread; abort() from worker should kill the process. */
    pause(); /* blocked until a signal — abort() terminates the whole process */
    /* Unreachable. */
}

static void test_p8_6(void) {
    TEST_BEGIN("P8.6   goc_pool_destroy() from own pool worker → abort()");
    bool got_sigabrt = fork_expect_sigabrt(p8_6_child_fn, NULL);
    ASSERT(got_sigabrt);
    TEST_PASS();
done:;
}

/* --- P8.7: goc_init() from non-main thread → abort() ------------------ */

static void* p8_7_non_main_init_thread(void* arg) {
    (void)arg;

    /* Must abort(): lifecycle init is main-thread only. */
    goc_init();
    return NULL;
}

static void p8_7_child_fn(void* arg) {
    (void)arg;

    pthread_t t;
    pthread_create(&t, NULL, p8_7_non_main_init_thread, NULL);

    /* Must not return: process should already be terminating via SIGABRT. */
    pthread_join(t, NULL);
}

static void test_p8_7(void) {
    TEST_BEGIN("P8.7   goc_init() from non-main pthread → abort()");
    bool got_sigabrt = fork_expect_sigabrt(p8_7_child_fn, NULL);
    ASSERT(got_sigabrt);
    TEST_PASS();
done:;
}

/* --- P8.8: goc_shutdown() from non-main thread → abort() -------------- */

static void* p8_8_non_main_shutdown_thread(void* arg) {
    (void)arg;

    /* Must abort(): lifecycle shutdown is main-thread only. */
    goc_shutdown();
    return NULL;
}

static void p8_8_child_fn(void* arg) {
    (void)arg;

    goc_init();

    pthread_t t;
    pthread_create(&t, NULL, p8_8_non_main_shutdown_thread, NULL);

    /* Must not return: process should already be terminating via SIGABRT. */
    pthread_join(t, NULL);
}

static void test_p8_8(void) {
    TEST_BEGIN("P8.8   goc_shutdown() from non-main pthread → abort()");
    bool got_sigabrt = fork_expect_sigabrt(p8_8_child_fn, NULL);
    ASSERT(got_sigabrt);
    TEST_PASS();
done:;
}

/* --- P8.9: goc_take_sync() from fiber context → abort() --------------- */

static void p8_9_take_sync_from_fiber(void* arg) {
    goc_chan* ch = (goc_chan*)arg;
    goc_take_sync(ch);
}

static void p8_9_child_fn(void* arg) {
    (void)arg;

    goc_chan* ch = goc_chan_make(0);
    goc_close(ch);

    goc_chan* done = goc_go(p8_9_take_sync_from_fiber, ch);
    goc_take_sync(done);
}

static void test_p8_9(void) {
    TEST_BEGIN("P8.9   goc_take_sync() from fiber context → abort()");
    bool got_sigabrt = fork_expect_sigabrt(p8_9_child_fn, NULL);
    ASSERT(got_sigabrt);
    TEST_PASS();
done:;
}

/* --- P8.10: goc_put_sync() from fiber context → abort() --------------- */

static void p8_10_put_sync_from_fiber(void* arg) {
    goc_chan* ch = (goc_chan*)arg;
    goc_put_sync(ch, goc_box_uint(0xBEAD));
}

static void p8_10_child_fn(void* arg) {
    (void)arg;

    goc_chan* ch = goc_chan_make(0);
    goc_close(ch);

    goc_chan* done = goc_go(p8_10_put_sync_from_fiber, ch);
    goc_take_sync(done);
}

static void test_p8_10(void) {
    TEST_BEGIN("P8.10  goc_put_sync() from fiber context → abort()");
    bool got_sigabrt = fork_expect_sigabrt(p8_10_child_fn, NULL);
    ASSERT(got_sigabrt);
    TEST_PASS();
done:;
}

/* --- P8.11: goc_alts_sync() from fiber context → abort() -------------- */

static void p8_11_alts_sync_from_fiber(void* arg) {
    (void)arg;

    goc_alt_op_t ops[1] = {
        { .op_kind = GOC_ALT_DEFAULT, .ch = NULL }
    };

    goc_alts_sync(ops, 1);
}

static void p8_11_child_fn(void* arg) {
    (void)arg;

    goc_chan* done = goc_go(p8_11_alts_sync_from_fiber, NULL);
    goc_take_sync(done);
}

static void test_p8_11(void) {
    TEST_BEGIN("P8.11  goc_alts_sync() from fiber context → abort()");
    bool got_sigabrt = fork_expect_sigabrt(p8_11_child_fn, NULL);
    ASSERT(got_sigabrt);
    TEST_PASS();
done:;
}

#else  /* _WIN32 — fork/waitpid not available; skip all P8 tests */

#define P8_SKIP(num, label) \
    static void test_p8_##num(void) { \
        TEST_BEGIN(label); \
        TEST_SKIP("fork/waitpid not available on Windows"); \
    done:; \
    }

P8_SKIP(1,  "P8.1   stack overflow: canary overwrite → abort()")
P8_SKIP(2,  "P8.2   goc_take() from OS thread → abort()")
P8_SKIP(3,  "P8.3   goc_put() from OS thread → abort()")
P8_SKIP(4,  "P8.4   goc_alts() with multiple defaults → abort()")
P8_SKIP(5,  "P8.5   goc_alts_sync() with multiple defaults → abort()")
P8_SKIP(6,  "P8.6   goc_pool_destroy() from own pool worker → abort()")
P8_SKIP(7,  "P8.7   goc_init() from non-main pthread → abort()")
P8_SKIP(8,  "P8.8   goc_shutdown() from non-main pthread → abort()")
P8_SKIP(9,  "P8.9   goc_take_sync() from fiber context → abort()")
P8_SKIP(10, "P8.10  goc_put_sync() from fiber context → abort()")
P8_SKIP(11, "P8.11  goc_alts_sync() from fiber context → abort()")

#undef P8_SKIP

#endif /* _WIN32 */

/* =========================================================================
 * main
 *
 * Initialises the runtime once in the parent, runs all Phase 8 tests in
 * order, shuts down the runtime, then prints a summary and exits with 0 on
 * success or 1 if any test failed.
 *
 * The crash handler is installed before goc_init() so that any unexpected
 * crash in the parent process (as opposed to a deliberately aborted child)
 * is reported with a backtrace.
 * ====================================================================== */

int main(void) {
    install_crash_handler();

    printf("libgoc test suite — Phase 8: Safety and crash behaviour\n");
    printf("=========================================================\n\n");

    goc_init();

    printf("Phase 8 — Safety and crash behaviour\n");
    test_p8_1();
    test_p8_2();
    test_p8_3();
    test_p8_4();
    test_p8_5();
    test_p8_6();
    test_p8_7();
    test_p8_8();
    test_p8_9();
    test_p8_10();
    test_p8_11();

    printf("\n");
    goc_shutdown();

    REPORT(g_tests_run, g_tests_passed, g_tests_failed);

    return (g_tests_failed == 0) ? 0 : 1;
}
