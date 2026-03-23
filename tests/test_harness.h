/*
 * tests/test_harness.h — Shared test harness for libgoc test suites
 *
 * Provides:
 *   - Result counters and TEST_BEGIN / ASSERT / TEST_PASS / TEST_FAIL macros
 *     (identical to the previously duplicated harness in each test file).
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

#if !defined(_WIN32) && !defined(__APPLE__)
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

/* =========================================================================
 * Portable nanosleep helper
 * ====================================================================== */

/**
 * goc_nanosleep() — sleep for ns nanoseconds (cross-platform).
 *
 * Converts nanoseconds to milliseconds (ceiling) and delegates to
 * uv_sleep(), which works on Linux, macOS, and Windows.
 */
static inline void goc_nanosleep(uint64_t ns) {
    uv_sleep((unsigned int)((ns + 999999ULL) / 1000000ULL));
}

/* =========================================================================
 * Result counters
 * ====================================================================== */

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

/* =========================================================================
 * Harness macros
 * ====================================================================== */

/* TEST_BEGIN — start a test case; print its name left-justified in a 50-char
 * column so that the trailing "pass" / "FAIL" tokens are aligned. */
#define TEST_BEGIN(name)                                        \
    do {                                                        \
        g_tests_run++;                                          \
        printf("  %-50s ", (name));                             \
        fflush(stdout);                                         \
    } while (0)

/* ASSERT — verify a condition; jump to `done:` on failure. */
#define ASSERT(cond)                                            \
    do {                                                        \
        if (!(cond)) {                                          \
            printf("FAIL\n    Assertion failed: %s\n"           \
                   "    %s:%d\n", #cond, __FILE__, __LINE__);   \
            g_tests_failed++;                                   \
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

/* TEST_FAIL — mark the current test as failed with a custom message. */
#define TEST_FAIL(msg)                                          \
    do {                                                        \
        printf("FAIL\n    %s\n    %s:%d\n",                     \
               (msg), __FILE__, __LINE__);                      \
        g_tests_failed++;                                       \
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
 * Crash handler
 * ====================================================================== */

#if !defined(_WIN32) && defined(GOC_HAVE_EXECINFO)

/* POSIX + glibc: full backtrace via execinfo */

#define GOC_BACKTRACE_MAX 64

static void crash_handler(int sig) {
    void* frames[GOC_BACKTRACE_MAX];
    int   n;

    /* Write a header to stderr.  dprintf is async-signal-safe on Linux.
     * On non-Linux POSIX (e.g. macOS) fprintf is used; it is not
     * async-signal-safe but works in practice for crash diagnostics. */
#if defined(__linux__)
    dprintf(STDERR_FILENO, "\n*** signal %d — backtrace ***\n", sig);
#else
    fprintf(stderr, "\n*** signal %d — backtrace ***\n", sig);
#endif

    n = backtrace(frames, GOC_BACKTRACE_MAX);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);

    /* Restore the default disposition and re-raise so the process exits
     * with the correct signal status (not 0 or 1). */
    struct sigaction sa = { .sa_handler = SIG_DFL };
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
    raise(sig);
}

static inline void install_crash_handler(void) {
    struct sigaction sa;
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    /* SA_RESETHAND: restore default after first invocation. */
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

#else  /* _WIN32 or no execinfo — portable fallback */

/* Simple handler: print signal number and re-raise with default. */
static void crash_handler(int sig) {
    fprintf(stderr, "\n*** signal %d ***\n", sig);
    fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}

static inline void install_crash_handler(void) {
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
}

#endif /* crash handler */

#endif /* GOC_TEST_HARNESS_H */
