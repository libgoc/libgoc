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
 * Compile requirements: -std=c11 -DGC_THREADS -D_GNU_SOURCE
 *
 * Dependencies:
 *   - libgoc (goc.h)  — runtime under test
 *   - Boehm GC        — must be the threaded variant (bdw-gc-threaded);
 *                        initialised internally by goc_init()
 *   - libuv           — event loop; drives fiber scheduling
 *   - POSIX fork / waitpid — used to isolate each abort()-inducing test in a
 *                        child process; the parent waits for the child and
 *                        verifies it was killed by SIGABRT
 *
 * Test isolation via fork:
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
#include <semaphore.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>

#include "test_harness.h"
#include "goc.h"
#include "minicoro.h"

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
         * The crash handler from the parent is NOT installed here — the child
         * must exit via abort() called by the runtime.
         */
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
 * overflow_then_yield — fiber entry point for P8.1.
 *
 * Simulates the effect of a stack overflow by directly zeroing the canary
 * word at stack_base — the lowest address in this fiber's 64 KB stack region,
 * where pool_worker_fn expects to find GOC_STACK_CANARY.
 *
 * Why not use real recursive overflow:
 *   Physically overflowing the stack via deep recursion is not reliable for
 *   this test.  The fiber stack is 64 KB; reaching the canary at the bottom
 *   requires ~128 × 512-byte frames (just over 64 KB total).  Any more and
 *   the stack pointer blows past the minicoro allocation boundary into
 *   unmapped memory, causing SIGSEGV in the child rather than the SIGABRT
 *   we are asserting.  Any less and the canary may not be reached on every
 *   platform and optimisation level.  The direct-write approach is
 *   semantically equivalent: regardless of what writes to the canary word
 *   (recursive overflow, buffer overrun, or explicit store), the observable
 *   state is identical — *stack_canary_ptr != GOC_STACK_CANARY — which is
 *   what pool_worker_fn checks.
 *
 * Sequence:
 *   1. Zero the canary word via mco_running()->stack_base.
 *   2. goc_put(ch, 1) — yields to the pool worker and parks the fiber.
 *   3. The worker re-enqueues the fiber; on the next iteration it checks the
 *      canary, finds it corrupted (0 != GOC_STACK_CANARY), and calls abort().
 *
 * arg — pointer to an unbuffered goc_chan used to synchronise with the
 *       parent (p8_1_child_fn).
 */
static void overflow_then_yield(void* c) {
    goc_chan* ch = (goc_chan*)c;

    /*
     * Corrupt the canary at the base of this fiber's stack.
     *
     * mco_running()->stack_base is the lowest mapped address of the 64 KB
     * fiber stack — the same pointer stored in entry->stack_canary_ptr by
     * goc_go_on immediately after mco_create.  Writing any value other than
     * GOC_STACK_CANARY here causes the pool worker to call abort() on the
     * next resume attempt.
     */
    mco_coro* self = mco_running();
    volatile uint32_t* canary = (volatile uint32_t*)self->stack_base;
    *canary = 0u;

    /*
     * Yield to the pool worker.  The worker re-enqueues this fiber, then on
     * the next loop iteration checks the canary before calling mco_resume —
     * finding 0 instead of GOC_STACK_CANARY — and calls abort().
     */
    goc_put(ch, (void*)1);
    /* Unreachable: abort() fires before mco_resume returns. */
}

static void p8_1_child_fn(void* arg) {
    (void)arg;

    goc_chan* ch = goc_chan_make(0);

    /* Fiber: corrupt the canary, then park on goc_put. */
    goc_go(overflow_then_yield, ch);

    /*
     * Block until the fiber yields (proof it reached and passed the canary-
     * corruption step).  The pool worker then re-schedules the fiber; the
     * canary check fires on that resume attempt → abort().
     *
     * If abort() does not fire the child exits normally with _exit(2) and the
     * parent records this as a test failure.
     */
    goc_take_sync(ch);

    /* Unreachable if abort() fires correctly. */
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
    bool got_sigabrt = fork_expect_sigabrt(p8_1_child_fn, NULL);
    ASSERT(got_sigabrt);
    TEST_PASS();
done:;
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
    goc_put(ch, (void*)(uintptr_t)0xCAFE);
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
    printf("\n");

    goc_shutdown();

    printf("=========================================================\n");
    printf("Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) {
        printf(", %d FAILED", g_tests_failed);
    }
    printf("\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
