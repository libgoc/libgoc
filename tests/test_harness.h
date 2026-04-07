/*
 * tests/test_harness.h — Shared test harness for libgoc test suites
 *
 * Provides:
 *   - Result counters and TEST_BEGIN / ASSERT / TEST_PASS / TEST_FAIL /
 *     TEST_SKIP macros.
 *   - GOC_STATS_DRAIN() — when GOC_ENABLE_STATS is defined, non-blockingly
 *     drains all pending events from goc_stats_chan_g and prints them to
 *     stderr.  Automatically called by ASSERT and TEST_FAIL on failure so
 *     every failing test shows the residual stats channel state without any
 *     per-test boilerplate.  Expands to ((void)0) when stats are disabled.
 *   - install_crash_handler() — registers a SIGSEGV / SIGABRT signal handler
 *     that (on Linux/macOS) prints a backtrace to stderr via
 *     backtrace_symbols_fd() and re-raises the signal; on Windows a simpler
 *     handler using signal() prints the signal number and re-raises.
 *     Call once at the top of main() before goc_init().
 *
 * Design notes (POSIX path):
 *   backtrace() / backtrace_symbols_fd() are async-signal-safe on glibc and
 *   write directly to a file descriptor, making them safe to call from a
 *   signal handler.  The output goes to stderr (fd 2) so it is captured by
 *   CTest's --output-on-failure regardless of stdout buffering.
 *
 *   The handler re-raises with the default disposition restored so the
 *   process still terminates with a signal (SIGSEGV / SIGABRT) rather than
 *   exit(1).  This preserves the correct exit status that CTest uses to mark
 *   the test as failed.
 *
 * Compile requirements: -g -rdynamic (or -fno-omit-frame-pointer)
 *   CMakeLists.txt builds with RelWithDebInfo which includes -g.
 *   -rdynamic must be added to test executables for symbol names to appear
 *   in the backtrace on Linux; see CMakeLists.txt.
 */

#ifndef GOC_TEST_HARNESS_H
#define GOC_TEST_HARNESS_H

#if defined(__linux__)
#  define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <uv.h>

#if !defined(_WIN32)
#  include <unistd.h>
#endif

#if defined(__has_include)
#  if __has_include(<execinfo.h>)
#    include <execinfo.h>
#    define GOC_HAVE_EXECINFO 1
#  endif
#endif

#ifdef LIBGOC_DEBUG
void goc_dbg_flush(void);
void goc_dbg_flush_signal_safe(void);
#endif

/* =========================================================================
 * Stats drain helper — GOC_STATS_DRAIN()
 *
 * Stats events are now delivered synchronously via a registered callback
 * (see goc_stats.h / goc_stats_set_callback).  There is no channel to
 * drain.  Individual test files collect events into their own buffers;
 * GOC_STATS_DRAIN is a no-op in the shared harness.
 * ====================================================================== */

#define GOC_STATS_DRAIN() ((void)0)

/* =========================================================================
 * Portable nanosleep helper
 * ====================================================================== */

static inline void goc_nanosleep(uint64_t ns) {
    uv_sleep((unsigned int)((ns + 999999ULL) / 1000000ULL));
}

/* =========================================================================
 * Result counters
 * ====================================================================== */

extern int g_tests_run;
extern int g_tests_passed;
extern int g_tests_failed;

#define REPORT(run, passed, failed)                            \
    do {                                                     \
        printf("\n==============================\n");           \
        printf("%d/%d tests passed", (passed), (run));       \
        if ((failed) > 0)                                     \
            printf(", %d FAILED", (failed));               \
        printf("\n");                                     \
    } while (0)

#if !defined(_WIN32)
#  define GOC_TEST_WATCHDOG_REARM() alarm(60)
#else
#  define GOC_TEST_WATCHDOG_REARM() ((void)0)
#endif

/* =========================================================================
 * Harness macros
 * ====================================================================== */

/* TEST_BEGIN — start a test case; print its name left-justified in a 50-char
 * column so that the trailing "pass" / "FAIL" tokens are aligned. */
#define TEST_BEGIN(name)                                        \
    do {                                                        \
        g_tests_run++;                                          \
    GOC_TEST_WATCHDOG_REARM();                             \
        GOC_DBG("TEST_BEGIN: %s\n", (name)); \
        printf("  %-50s ", (name));                             \
        fflush(stdout);                                         \
    } while (0)

/* ASSERT — verify a condition; on failure print the location, drain any
 * pending stats events to stderr, then jump to `done:`. */
#define ASSERT(cond)                                            \
    do {                                                        \
        if (!(cond)) {                                          \
            printf("FAIL\n    Assertion failed: %s\n"           \
                   "    %s:%d\n", #cond, __FILE__, __LINE__);   \
            g_tests_failed++;                                   \
            GOC_STATS_DRAIN();                                  \
            goto done;                                          \
        }                                                       \
    } while (0)

/* TEST_PASS — mark the current test as passed and exit the test function. */
#define TEST_PASS()                                             \
    do {                                                        \
        printf("pass\n");                                       \
        g_tests_passed++;                                       \
        goto done;                                              \
    } while (0)

/* TEST_FAIL — mark the current test as failed with a custom message; drain
 * any pending stats events to stderr, then jump to `done:`. */
#define TEST_FAIL(msg)                                          \
    do {                                                        \
        printf("FAIL\n    %s\n    %s:%d\n",                     \
               (msg), __FILE__, __LINE__);                      \
        g_tests_failed++;                                       \
        GOC_STATS_DRAIN();                                      \
        goto done;                                              \
    } while (0)

/* TEST_SKIP — mark a test as skipped (not run on this platform). */
#define TEST_SKIP(reason)                                       \
    do {                                                        \
        printf("skip (%s)\n", (reason));                        \
        g_tests_run--;                                          \
        goto done;                                              \
    } while (0)

/* =========================================================================
 * Crash and termination handlers
 *
 * Signals handled:
 *   SIGSEGV / SIGABRT — hard crash: print backtrace, dump stats, re-raise.
 *   SIGTERM           — external kill (e.g. CTest timeout): dump stats,
 *                       re-raise so the process exits with the correct
 *                       signal status.
 *   SIGALRM           — watchdog timeout: fired by goc_test_arm_watchdog()
 *                       if the test process hangs.  Prints a hang diagnostic,
 *                       dumps stats, then terminates via SIGABRT so CTest
 *                       records a failure with a core dump.
 *
 * goc_test_arm_watchdog(secs) — call once in main() after
 *   install_crash_handler() to arm a self-delivery SIGALRM after `secs`
 *   seconds.  If the test hangs (e.g. blocked forever in goc_take_sync due
 *   to a race condition), the alarm fires, the handler dumps the pending
 *   stats channel, and the process terminates.  No-op on Windows.
 *
 * Note on async-signal safety: goc_stats_drain_pending() is not
 * async-signal-safe (it calls goc_take_try which may acquire locks).
 * This is intentionally accepted in the test harness: the process is
 * already dying, and the drain produces the most useful post-mortem
 * information.  Do not use this pattern in production signal handlers.
 * ====================================================================== */

#if !defined(_WIN32) && defined(GOC_HAVE_EXECINFO)

/* POSIX + glibc: full backtrace via execinfo */

#define GOC_BACKTRACE_MAX 64

/* crash_handler — SIGSEGV, SIGABRT, SIGTERM */
static void crash_handler(int sig) {
    void* frames[GOC_BACKTRACE_MAX];
    int   n;

#if defined(__linux__)
    dprintf(STDERR_FILENO, "\n*** signal %d — backtrace ***\n", sig);
#else
    fprintf(stderr, "\n*** signal %d — backtrace ***\n", sig);
#endif

    n = backtrace(frames, GOC_BACKTRACE_MAX);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);

#ifdef LIBGOC_DEBUG
    goc_dbg_flush_signal_safe();
#endif
    GOC_STATS_DRAIN();

    struct sigaction sa = { .sa_handler = SIG_DFL };
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
    raise(sig);
}

/* watchdog_handler — SIGALRM: fired when the test process hangs */
static void watchdog_handler(int sig) {
    (void)sig;
#if defined(__linux__)
    dprintf(STDERR_FILENO,
            "\n*** watchdog timeout (SIGALRM) — possible hang or deadlock ***\n");
#else
    fprintf(stderr,
            "\n*** watchdog timeout (SIGALRM) — possible hang or deadlock ***\n");
#endif

#ifdef LIBGOC_DEBUG
    goc_dbg_flush_signal_safe();
#endif
    GOC_STATS_DRAIN();

    /* Terminate via SIGABRT so CTest records a failure and a core is dumped. */
    struct sigaction sa = { .sa_handler = SIG_DFL };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGABRT, &sa, NULL);
    raise(SIGABRT);
}

static inline void install_crash_handler(void) {
#ifdef LIBGOC_DEBUG
    atexit(goc_dbg_flush);
#endif
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;

    sa.sa_handler = crash_handler;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    sa.sa_handler = watchdog_handler;
    sigaction(SIGALRM, &sa, NULL);
}

static inline void goc_test_arm_watchdog(unsigned secs) {
    alarm(secs);
}

#else  /* _WIN32 or no execinfo — portable fallback */

static void crash_handler(int sig) {
    fprintf(stderr, "\n*** signal %d ***\n", sig);
    fflush(stderr);
#ifdef LIBGOC_DEBUG
    goc_dbg_flush_signal_safe();
#endif
    GOC_STATS_DRAIN();
    signal(sig, SIG_DFL);
    raise(sig);
}

static inline void install_crash_handler(void) {
#ifdef LIBGOC_DEBUG
    atexit(goc_dbg_flush);
#endif
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGTERM, crash_handler);
}

static inline void goc_test_arm_watchdog(unsigned secs) {
    (void)secs; /* alarm() not available on Windows */
}

#endif /* crash handler */

#endif /* GOC_TEST_HARNESS_H */
