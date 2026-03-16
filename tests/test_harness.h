/*
 * tests/test_harness.h — Shared test harness for libgoc test suites
 *
 * Provides:
 *   - Result counters and TEST_BEGIN / ASSERT / TEST_PASS / TEST_FAIL macros
 *     (identical to the previously duplicated harness in each test file).
 *   - install_crash_handler() — registers a SIGSEGV / SIGABRT signal handler
 *     that prints a backtrace to stderr via backtrace_symbols_fd() and
 *     re-raises the signal so the process exits with the correct status.
 *     Call once at the top of main() before goc_init().
 *
 * Design notes:
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
 *   in the backtrace; see CMakeLists.txt.
 */

#ifndef GOC_TEST_HARNESS_H
#define GOC_TEST_HARNESS_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

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

/* =========================================================================
 * Crash handler
 * ====================================================================== */

#define GOC_BACKTRACE_MAX 64

static void crash_handler(int sig) {
    void*  frames[GOC_BACKTRACE_MAX];
    int    n;

    /* Write a header to stderr.  dprintf is async-signal-safe. */
    dprintf(STDERR_FILENO, "\n*** signal %d — backtrace ***\n", sig);

    n = backtrace(frames, GOC_BACKTRACE_MAX);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);

    /* Restore the default disposition and re-raise so the process exits
     * with the correct signal status (not 0 or 1). */
    struct sigaction sa = { .sa_handler = SIG_DFL };
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
    raise(sig);
}

/*
 * install_crash_handler — register crash_handler for SIGSEGV and SIGABRT.
 * Call once at the top of main(), before goc_init().
 */
static inline void install_crash_handler(void) {
    struct sigaction sa;
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    /* SA_RESETHAND: restore default after first invocation (belt-and-braces,
     * since we also restore manually inside the handler). */
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

#endif /* GOC_TEST_HARNESS_H */
