/*
 * tests/test_p9_mutexes.c — Phase 9: RW mutex tests for libgoc
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#include "test_harness.h"
#include "goc.h"

/*
 * done_t — a portable semaphore built from a mutex + condvar.
 * sem_timedwait is not available on macOS; this implementation works on
 * Linux, macOS and Windows (MSYS2/MinGW with libwinpthread).
 * The flag is reset after each successful wait, making done_t reusable.
 */
typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    int             flag;
} done_t;

static void done_init(done_t* d) {
    pthread_mutex_init(&d->mtx, NULL);
    pthread_cond_init(&d->cond, NULL);
    d->flag = 0;
}
static void done_signal(done_t* d) {
    pthread_mutex_lock(&d->mtx);
    d->flag = 1;
    pthread_cond_signal(&d->cond);
    pthread_mutex_unlock(&d->mtx);
}
static void done_wait(done_t* d) {
    pthread_mutex_lock(&d->mtx);
    while (!d->flag)
        pthread_cond_wait(&d->cond, &d->mtx);
    d->flag = 0;
    pthread_mutex_unlock(&d->mtx);
}
static void done_destroy(done_t* d) {
    pthread_mutex_destroy(&d->mtx);
    pthread_cond_destroy(&d->cond);
}

static bool done_wait_ms(done_t* d, long ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    long nsec = ts.tv_nsec + (ms % 1000) * 1000000L;
    ts.tv_sec += (ms / 1000) + (nsec / 1000000000L);
    ts.tv_nsec = nsec % 1000000000L;

    pthread_mutex_lock(&d->mtx);
    while (!d->flag) {
        int r = pthread_cond_timedwait(&d->cond, &d->mtx, &ts);
        if (r == ETIMEDOUT) {
            pthread_mutex_unlock(&d->mtx);
            return false;
        }
    }
    d->flag = 0;
    pthread_mutex_unlock(&d->mtx);
    return true;
}

static void test_p9_1(void)
{
    TEST_BEGIN("P9.1  read lock acquire+release from OS thread");

    goc_mutex* mx = goc_mutex_make();
    ASSERT(mx != NULL);

    goc_chan* lk = goc_read_lock(mx);
    ASSERT(lk != NULL);

    goc_val_t* v = goc_take_sync(lk);
    ASSERT(v->ok == GOC_OK);

    goc_close(lk);
    TEST_PASS();
 done:;
}

static void test_p9_2(void)
{
    TEST_BEGIN("P9.2  multiple readers acquire concurrently");

    goc_mutex* mx = goc_mutex_make();
    ASSERT(mx != NULL);

    goc_chan* r1 = goc_read_lock(mx);
    goc_chan* r2 = goc_read_lock(mx);
    ASSERT(r1 != NULL);
    ASSERT(r2 != NULL);

    goc_val_t* v1 = goc_take_sync(r1);
    goc_val_t* v2 = goc_take_sync(r2);
    ASSERT(v1->ok == GOC_OK);
    ASSERT(v2->ok == GOC_OK);

    goc_close(r1);
    goc_close(r2);

    TEST_PASS();
 done:;
}

typedef struct {
    goc_mutex* mx;
    goc_chan*  lock_ch;
    goc_val_t* acquired;
    done_t*    started;
    done_t*    acquired_sem;
} writer_thread_args_t;

static void* writer_wait_thread(void* arg)
{
    writer_thread_args_t* a = (writer_thread_args_t*)arg;
    a->lock_ch = goc_write_lock(a->mx);
    done_signal(a->started);
    a->acquired = goc_take_sync(a->lock_ch);
    done_signal(a->acquired_sem);
    return NULL;
}

static void test_p9_3(void)
{
    TEST_BEGIN("P9.3  writer blocks until reader releases");

    goc_mutex* mx = goc_mutex_make();
    ASSERT(mx != NULL);

    goc_chan* r = goc_read_lock(mx);
    ASSERT(r != NULL);
    ASSERT(goc_take_sync(r)->ok == GOC_OK);

    done_t started, acquired;
    done_init(&started);
    done_init(&acquired);

    writer_thread_args_t args = {
        .mx = mx,
        .started = &started,
        .acquired_sem = &acquired,
    };

    pthread_t tid;
    pthread_create(&tid, NULL, writer_wait_thread, &args);

    done_wait(&started);
    ASSERT(done_wait_ms(&acquired, 20) == false);

    goc_close(r);

    ASSERT(done_wait_ms(&acquired, 200) == true);
    ASSERT(args.acquired->ok == GOC_OK);

    goc_close(args.lock_ch);

    pthread_join(tid, NULL);
    done_destroy(&started);
    done_destroy(&acquired);

    TEST_PASS();
 done:;
}

static void test_p9_4(void)
{
    TEST_BEGIN("P9.4  queued writer blocks subsequent readers");

    goc_mutex* mx = goc_mutex_make();
    ASSERT(mx != NULL);

    goc_chan* r1 = goc_read_lock(mx);
    ASSERT(r1 != NULL);
    ASSERT(goc_take_sync(r1)->ok == GOC_OK);

    goc_chan* w1 = goc_write_lock(mx);
    ASSERT(w1 != NULL);

    goc_chan* r2 = goc_read_lock(mx);
    ASSERT(r2 != NULL);

    goc_val_t* try_r2_before = goc_take_try(r2);
    ASSERT(try_r2_before->ok == GOC_EMPTY);

    goc_close(r1);

    goc_val_t* wv = goc_take_sync(w1);
    ASSERT(wv->ok == GOC_OK);

    goc_val_t* try_r2_during = goc_take_try(r2);
    ASSERT(try_r2_during->ok == GOC_EMPTY);

    goc_close(w1);

    goc_val_t* r2v = goc_take_sync(r2);
    ASSERT(r2v->ok == GOC_OK);

    goc_close(r2);

    TEST_PASS();
 done:;
}

typedef struct {
    goc_mutex* mx;
    goc_status_t ok;
    done_t* done;
} fiber_wait_reader_args_t;

static void fiber_wait_reader(void* arg)
{
    fiber_wait_reader_args_t* a = (fiber_wait_reader_args_t*)arg;
    goc_chan* r = goc_read_lock(a->mx);
    goc_val_t* v = goc_take(r);
    a->ok = v->ok;
    done_signal(a->done);
    goc_close(r);
}

static void test_p9_5(void)
{
    TEST_BEGIN("P9.5  fiber parks on read lock behind writer");

    goc_mutex* mx = goc_mutex_make();
    ASSERT(mx != NULL);

    goc_chan* w = goc_write_lock(mx);
    ASSERT(w != NULL);
    ASSERT(goc_take_sync(w)->ok == GOC_OK);

    done_t done;
    done_init(&done);

    fiber_wait_reader_args_t args = {
        .mx = mx,
        .ok = GOC_EMPTY,
        .done = &done,
    };

    goc_chan* join = goc_go(fiber_wait_reader, &args);
    ASSERT(join != NULL);

    ASSERT(done_wait_ms(&done, 20) == false);

    goc_close(w);

    ASSERT(done_wait_ms(&done, 200) == true);
    ASSERT(args.ok == GOC_OK);

    goc_val_t* jv = goc_take_sync(join);
    ASSERT(jv->ok == GOC_CLOSED);

    done_destroy(&done);
    TEST_PASS();
 done:;
}

int main(void)
{
    install_crash_handler();

    printf("libgoc test suite — Phase 9: RW mutexes\n");
    printf("=========================================\n\n");

    goc_init();

    printf("Phase 9 — RW mutexes\n");
    test_p9_1();
    test_p9_2();
    test_p9_3();
    test_p9_4();
    test_p9_5();
    printf("\n");

    goc_shutdown();

    printf("=========================================\n");
    printf("Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0)
        printf(", %d FAILED", g_tests_failed);
    printf("\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
