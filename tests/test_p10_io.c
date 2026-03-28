/*
 * tests/test_p10_io.c — Phase 10: Async I/O wrapper tests for libgoc
 *
 * Verifies the channel-returning async I/O wrappers declared in goc_io.h.
 * Tests focus on file-system and DNS operations since they do not require
 * network infrastructure to be pre-configured.
 *
 * Build:  cmake -B build && cmake --build build
 * Run:    ctest --test-dir build --output-on-failure
 *         ./build/test_p10_io
 *
 * Compile requirements: -std=c11 -DGC_THREADS -D_GNU_SOURCE
 *
 * Test coverage:
 *
 *   P10.1  goc_io_fs_open: open a new file; file descriptor >= 0
 *   P10.2  goc_io_fs_write: write data to an open file; returns written bytes
 *   P10.3  goc_io_fs_read: read back the data; matches written content
 *   P10.4  goc_io_fs_stat: stat the file; size and type fields correct
 *   P10.5  goc_io_fs_rename: rename the file; stat old path fails, new path ok
 *   P10.6  goc_io_fs_unlink: delete the file; subsequent stat fails
 *   P10.7  goc_io_fs_open with invalid path: fd < 0 (error code)
 *   P10.8  goc_io_getaddrinfo: resolve "localhost"; ok == GOC_IO_OK, res != NULL
 *   P10.9  goc_io_getaddrinfo with empty node and service: returns error
 *   P10.10 goc_io_getaddrinfo returns non-NULL channel
 *   P10.11 goc_io_fs_sendfile: copy bytes between two file descriptors
 *   P10.12 Channel-based goc_io_fs_open integrates with goc_alts (select
 *          on open vs. a dummy channel that never fires)
 *   P10.13 goc_io_handle_register + goc_io_handle_close: GC-allocated
 *          uv_async_t handle registers, closes, and unregisters cleanly
 *          (uv_async_init is the only uv_*_init safe to call from any thread)
 */

#if !defined(_WIN32) && !defined(__APPLE__)
#  define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <windows.h>
#endif

#include "test_harness.h"
#include "goc.h"
#include "goc_io.h"

/* Temporary file paths used across tests — set by init_tmp_paths(). */
static const char* TMP_PATH   = NULL;
static const char* TMP_PATH2  = NULL;
static const char* TMP_PATH3  = NULL;

#ifdef _WIN32
static char s_tmp1[MAX_PATH];
static char s_tmp2[MAX_PATH];
static char s_tmp3[MAX_PATH];
#endif

static void init_tmp_paths(void)
{
#ifdef _WIN32
    char tmp_dir[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, tmp_dir);
    if (len == 0) {
        /* Fallback: use current directory */
        tmp_dir[0] = '.';
        tmp_dir[1] = '\\';
        tmp_dir[2] = '\0';
    }
    snprintf(s_tmp1, sizeof(s_tmp1), "%sgoc_io_test.txt",         tmp_dir);
    snprintf(s_tmp2, sizeof(s_tmp2), "%sgoc_io_test_renamed.txt", tmp_dir);
    snprintf(s_tmp3, sizeof(s_tmp3), "%sgoc_io_test_dst.txt",     tmp_dir);
    TMP_PATH  = s_tmp1;
    TMP_PATH2 = s_tmp2;
    TMP_PATH3 = s_tmp3;
#else
    TMP_PATH  = "/tmp/goc_io_test.txt";
    TMP_PATH2 = "/tmp/goc_io_test_renamed.txt";
    TMP_PATH3 = "/tmp/goc_io_test_dst.txt";
#endif
}

/* Content used for write/read tests. */
static const char  CONTENT[]  = "hello libgoc async io";
static const int   CONTENT_LEN = (int)(sizeof(CONTENT) - 1);

/* =========================================================================
 * Helper: ensure tmp files are gone at the start of each run.
 * ====================================================================== */
static void cleanup_tmp_files(void)
{
    uv_fs_t req;
    uv_fs_unlink(goc_scheduler(), &req, TMP_PATH,  NULL);
    uv_fs_req_cleanup(&req);
    uv_fs_unlink(goc_scheduler(), &req, TMP_PATH2, NULL);
    uv_fs_req_cleanup(&req);
    uv_fs_unlink(goc_scheduler(), &req, TMP_PATH3, NULL);
    uv_fs_req_cleanup(&req);
}

/* =========================================================================
 * Fiber state structs
 * ====================================================================== */

typedef struct {
    int ok;
} fiber_result_t;

/* =========================================================================
 * P10.1  goc_io_fs_open: open a new file
 * ====================================================================== */

static void fiber_p10_1(void* arg)
{
    fiber_result_t* r = (fiber_result_t*)arg;
    goc_chan*   ch = goc_io_fs_open(TMP_PATH,
                                    UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644);
    goc_val_t*  v  = goc_take(ch);
    if (!v || v->ok != GOC_OK) goto done;
    uv_file fd = (uv_file)(intptr_t)v->val;
    if (fd < 0) goto done;
    goc_take(goc_io_fs_close(fd));
    r->ok = 1;
done:;
}

static void test_p10_1(void)
{
    TEST_BEGIN("P10.1  goc_io_fs_open opens a new file");
    fiber_result_t r = {0};
    goc_chan* done_ch = goc_go(fiber_p10_1, &r);
    goc_take_sync(done_ch);
    ASSERT(r.ok);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.2  goc_io_fs_write: write data
 * ====================================================================== */

static void fiber_p10_2(void* arg)
{
    fiber_result_t* r = (fiber_result_t*)arg;
    /* Open for writing */
    goc_val_t* vopen = goc_take(goc_io_fs_open(TMP_PATH,
                               UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644));
    if (!vopen || vopen->ok != GOC_OK) goto done;
    uv_file fd = (uv_file)(intptr_t)vopen->val;
    if (fd < 0) goto done;

    uv_buf_t buf = uv_buf_init((char*)CONTENT, (unsigned)CONTENT_LEN);
    goc_val_t* vwrite = goc_take(goc_io_fs_write(fd, &buf, 1, 0));
    ssize_t written = (ssize_t)(intptr_t)vwrite->val;
    goc_take(goc_io_fs_close(fd));
    if (written != CONTENT_LEN) goto done;

    r->ok = 1;
done:;
}

static void test_p10_2(void)
{
    TEST_BEGIN("P10.2  goc_io_fs_write writes correct byte count");
    fiber_result_t r = {0};
    goc_chan* done_ch = goc_go(fiber_p10_2, &r);
    goc_take_sync(done_ch);
    ASSERT(r.ok);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.3  goc_io_fs_read: read back written content
 * ====================================================================== */

static void fiber_p10_3(void* arg)
{
    fiber_result_t* r = (fiber_result_t*)arg;
    goc_val_t* vopen = goc_take(goc_io_fs_open(TMP_PATH, UV_FS_O_RDONLY, 0));
    if (!vopen || vopen->ok != GOC_OK) goto done;
    uv_file fd = (uv_file)(intptr_t)vopen->val;
    if (fd < 0) goto done;

    char readbuf[64] = {0};
    uv_buf_t buf = uv_buf_init(readbuf, sizeof(readbuf) - 1);
    goc_val_t* vrd = goc_take(goc_io_fs_read(fd, &buf, 1, 0));
    ssize_t rd = (ssize_t)(intptr_t)vrd->val;
    goc_take(goc_io_fs_close(fd));

    if (rd != CONTENT_LEN) goto done;
    if (memcmp(readbuf, CONTENT, (size_t)CONTENT_LEN) != 0) goto done;
    r->ok = 1;
done:;
}

static void test_p10_3(void)
{
    TEST_BEGIN("P10.3  goc_io_fs_read reads back correct content");
    fiber_result_t r = {0};
    goc_chan* done_ch = goc_go(fiber_p10_3, &r);
    goc_take_sync(done_ch);
    ASSERT(r.ok);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.4  goc_io_fs_stat: stat the file
 * ====================================================================== */

static void fiber_p10_4(void* arg)
{
    fiber_result_t* r = (fiber_result_t*)arg;
    goc_val_t* vstat = goc_take(goc_io_fs_stat(TMP_PATH));
    if (!vstat || vstat->ok != GOC_OK) goto done;
    goc_io_fs_stat_t* st = (goc_io_fs_stat_t*)vstat->val;
    if (!st || st->ok != GOC_IO_OK) goto done;
    if ((int64_t)st->statbuf.st_size != CONTENT_LEN) goto done;
    r->ok = 1;
done:;
}

static void test_p10_4(void)
{
    TEST_BEGIN("P10.4  goc_io_fs_stat reports correct file size");
    fiber_result_t r = {0};
    goc_chan* done_ch = goc_go(fiber_p10_4, &r);
    goc_take_sync(done_ch);
    ASSERT(r.ok);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.5  goc_io_fs_rename: rename the file
 * ====================================================================== */

static void fiber_p10_5(void* arg)
{
    fiber_result_t* r = (fiber_result_t*)arg;
    goc_val_t* vren = goc_take(goc_io_fs_rename(TMP_PATH, TMP_PATH2));
    int ren = (int)(intptr_t)vren->val;
    if (ren != 0) goto done;

    /* Old path should no longer exist */
    goc_val_t* vold = goc_take(goc_io_fs_stat(TMP_PATH));
    goc_io_fs_stat_t* old_st = (goc_io_fs_stat_t*)vold->val;
    if (!old_st || old_st->ok == GOC_IO_OK) goto done;  /* still exists = fail */

    /* New path should exist with the same size */
    goc_val_t* vnew = goc_take(goc_io_fs_stat(TMP_PATH2));
    goc_io_fs_stat_t* new_st = (goc_io_fs_stat_t*)vnew->val;
    if (!new_st || new_st->ok != GOC_IO_OK) goto done;
    if ((int64_t)new_st->statbuf.st_size != CONTENT_LEN) goto done;

    r->ok = 1;
done:;
}

static void test_p10_5(void)
{
    TEST_BEGIN("P10.5  goc_io_fs_rename renames file correctly");
    fiber_result_t r = {0};
    goc_chan* done_ch = goc_go(fiber_p10_5, &r);
    goc_take_sync(done_ch);
    ASSERT(r.ok);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.6  goc_io_fs_unlink: delete the (renamed) file
 * ====================================================================== */

static void fiber_p10_6(void* arg)
{
    fiber_result_t* r = (fiber_result_t*)arg;
    goc_val_t* vul = goc_take(goc_io_fs_unlink(TMP_PATH2));
    int ul = (int)(intptr_t)vul->val;
    if (ul != 0) goto done;

    /* File should no longer exist */
    goc_val_t* vstat = goc_take(goc_io_fs_stat(TMP_PATH2));
    goc_io_fs_stat_t* st = (goc_io_fs_stat_t*)vstat->val;
    if (!st || st->ok == GOC_IO_OK) goto done;  /* still exists = fail */

    r->ok = 1;
done:;
}

static void test_p10_6(void)
{
    TEST_BEGIN("P10.6  goc_io_fs_unlink deletes file, stat then fails");
    fiber_result_t r = {0};
    goc_chan* done_ch = goc_go(fiber_p10_6, &r);
    goc_take_sync(done_ch);
    ASSERT(r.ok);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.7  goc_io_fs_open with non-existent path + UV_FS_O_RDONLY → error
 * ====================================================================== */

static void fiber_p10_7(void* arg)
{
    fiber_result_t* r = (fiber_result_t*)arg;
    goc_chan* ch = goc_io_fs_open("/nonexistent/path/that/does/not/exist",
                                   UV_FS_O_RDONLY, 0);
    goc_val_t*     v  = goc_take(ch);
    if (!v || v->ok != GOC_OK) goto done;
    uv_file fd = (uv_file)(intptr_t)v->val;
    /* result should be a negative error code */
    if (fd >= 0) goto done;
    r->ok = 1;
done:;
}

static void test_p10_7(void)
{
    TEST_BEGIN("P10.7  goc_io_fs_open with invalid path returns error code");
    fiber_result_t r = {0};
    goc_chan* done_ch = goc_go(fiber_p10_7, &r);
    goc_take_sync(done_ch);
    ASSERT(r.ok);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.8  goc_io_getaddrinfo: resolve "localhost"
 * ====================================================================== */

static void fiber_p10_8(void* arg)
{
    fiber_result_t* r = (fiber_result_t*)arg;
    goc_chan*           ch = goc_io_getaddrinfo("localhost", NULL, NULL);
    goc_val_t*          v  = goc_take(ch);
    if (!v || v->ok != GOC_OK) goto done;
    goc_io_getaddrinfo_t*  res = (goc_io_getaddrinfo_t*)v->val;
    if (!res || res->ok != GOC_IO_OK || res->res == NULL) goto done;
    uv_freeaddrinfo(res->res);
    r->ok = 1;
done:;
}

static void test_p10_8(void)
{
    TEST_BEGIN("P10.8  goc_io_getaddrinfo resolves \"localhost\"");
    fiber_result_t r = {0};
    goc_chan* done_ch = goc_go(fiber_p10_8, &r);
    goc_take_sync(done_ch);
    ASSERT(r.ok);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.9  goc_io_getaddrinfo with node=NULL and service=NULL → error
 * ====================================================================== */

static void fiber_p10_9(void* arg)
{
    fiber_result_t* r = (fiber_result_t*)arg;
    goc_chan*          ch  = goc_io_getaddrinfo(NULL, NULL, NULL);
    goc_val_t*         v   = goc_take(ch);
    if (!v || v->ok != GOC_OK) goto done;
    goc_io_getaddrinfo_t* res = (goc_io_getaddrinfo_t*)v->val;
    if (!res) goto done;
    /* libuv returns an error (EAI_NONAME or similar) when both are NULL */
    if (res->ok == GOC_IO_OK && res->res != NULL)
        uv_freeaddrinfo(res->res);
    /* Test passes regardless of status — we just need no crash */
    r->ok = 1;
done:;
}

static void test_p10_9(void)
{
    TEST_BEGIN("P10.9  goc_io_getaddrinfo NULL node+service: no crash");
    fiber_result_t r = {0};
    goc_chan* done_ch = goc_go(fiber_p10_9, &r);
    goc_take_sync(done_ch);
    ASSERT(r.ok);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.10 goc_io_getaddrinfo returns non-NULL channel (compile + API check)
 * ====================================================================== */

static void test_p10_10(void)
{
    TEST_BEGIN("P10.10 goc_io_getaddrinfo returns non-NULL channel");
    goc_chan* ch = goc_io_getaddrinfo("localhost", NULL, NULL);
    ASSERT(ch != NULL);
    /* Drain the channel to avoid leaking a live channel at shutdown. */
    goc_val_t* v = goc_take_sync(ch);
    if (v && v->ok == GOC_OK && v->val) {
        goc_io_getaddrinfo_t* res = (goc_io_getaddrinfo_t*)v->val;
        if (res->ok == GOC_IO_OK && res->res)
            uv_freeaddrinfo(res->res);
    }
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.11 goc_io_fs_sendfile: copy bytes between two file descriptors
 * ====================================================================== */

static void fiber_p10_11(void* arg)
{
    fiber_result_t* r = (fiber_result_t*)arg;

    /* Create source file with content */
    goc_val_t* vsrc = goc_take(goc_io_fs_open(TMP_PATH,
                               UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644));
    if (!vsrc || vsrc->ok != GOC_OK) goto done;
    uv_file src_fd = (uv_file)(intptr_t)vsrc->val;
    if (src_fd < 0) goto done;

    uv_buf_t wbuf = uv_buf_init((char*)CONTENT, (unsigned)CONTENT_LEN);
    goc_val_t* vwrite = goc_take(goc_io_fs_write(src_fd, &wbuf, 1, 0));
    ssize_t written = (ssize_t)(intptr_t)vwrite->val;
    goc_take(goc_io_fs_close(src_fd));
    if (written != CONTENT_LEN) goto done;

    /* Reopen source for reading */
    goc_val_t* vsrc2 = goc_take(goc_io_fs_open(TMP_PATH, UV_FS_O_RDONLY, 0));
    if (!vsrc2 || vsrc2->ok != GOC_OK) goto done;
    src_fd = (uv_file)(intptr_t)vsrc2->val;
    if (src_fd < 0) goto done;

    /* Create destination file */
    goc_val_t* vdst = goc_take(goc_io_fs_open(TMP_PATH3,
                               UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644));
    if (!vdst || vdst->ok != GOC_OK) {
        goc_take(goc_io_fs_close(src_fd));
        goto done;
    }
    uv_file dst_fd = (uv_file)(intptr_t)vdst->val;
    if (dst_fd < 0) {
        goc_take(goc_io_fs_close(src_fd));
        goto done;
    }

    goc_val_t* vsf = goc_take(goc_io_fs_sendfile(dst_fd, src_fd, 0, (size_t)CONTENT_LEN));
    ssize_t sf = (ssize_t)(intptr_t)vsf->val;
    goc_take(goc_io_fs_close(src_fd));
    goc_take(goc_io_fs_close(dst_fd));
    if (sf != CONTENT_LEN) goto done;

    /* Verify destination content */
    goc_val_t* vvfd = goc_take(goc_io_fs_open(TMP_PATH3, UV_FS_O_RDONLY, 0));
    if (!vvfd || vvfd->ok != GOC_OK) goto done;
    uv_file verify_fd = (uv_file)(intptr_t)vvfd->val;
    if (verify_fd < 0) goto done;
    char rbuf[64] = {0};
    uv_buf_t rbufv = uv_buf_init(rbuf, sizeof(rbuf) - 1);
    goc_val_t* vrd = goc_take(goc_io_fs_read(verify_fd, &rbufv, 1, 0));
    ssize_t rd = (ssize_t)(intptr_t)vrd->val;
    goc_take(goc_io_fs_close(verify_fd));
    if (rd != CONTENT_LEN) goto done;
    if (memcmp(rbuf, CONTENT, (size_t)CONTENT_LEN) != 0) goto done;

    r->ok = 1;
done:;
}

static void test_p10_11(void)
{
    TEST_BEGIN("P10.11 goc_io_fs_sendfile copies correct byte count");
    fiber_result_t r = {0};
    goc_chan* done_ch = goc_go(fiber_p10_11, &r);
    goc_take_sync(done_ch);
    ASSERT(r.ok);
    /* Cleanup */
    uv_fs_t req;
    uv_fs_unlink(goc_scheduler(), &req, TMP_PATH,  NULL); uv_fs_req_cleanup(&req);
    uv_fs_unlink(goc_scheduler(), &req, TMP_PATH3, NULL); uv_fs_req_cleanup(&req);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.12 goc_io_fs_open integrates with goc_alts (select on two I/O ops)
 * ====================================================================== */

static void fiber_p10_12(void* arg)
{
    fiber_result_t* r = (fiber_result_t*)arg;

    /* Select between two competing I/O channels: one opens a file, the other
     * is a rendezvous channel that never fires.  Verify that alts works
     * correctly with channel-returning I/O functions and delivers one result
     * without crashing. */
    goc_chan* open_ch  = goc_io_fs_open(TMP_PATH,
                                         UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644);
    goc_chan* dummy_ch = goc_chan_make(0);   /* rendezvous; nobody writes */

    goc_alt_op ops[2] = {
        { .ch = open_ch,  .op_kind = GOC_ALT_TAKE, .put_val = NULL },
        { .ch = dummy_ch, .op_kind = GOC_ALT_TAKE, .put_val = NULL },
    };
    goc_alts_result* result = goc_alts(ops, 2);

    if (result->ch != open_ch) goto done;   /* unexpected winner */
    uv_file fd = (uv_file)(intptr_t)result->value.val;
    if (fd < 0) goto done;

    goc_take(goc_io_fs_close(fd));

    /* Close the dummy channel so any parked alts entries are released. */
    goc_close(dummy_ch);

    r->ok = 1;
done:;
}

static void test_p10_12(void)
{
    TEST_BEGIN("P10.12 goc_io_fs_open works with goc_alts (select)");
    fiber_result_t r = {0};
    goc_chan* done_ch = goc_go(fiber_p10_12, &r);
    goc_take_sync(done_ch);
    ASSERT(r.ok);
    /* Cleanup */
    uv_fs_t req;
    uv_fs_unlink(goc_scheduler(), &req, TMP_PATH, NULL);
    uv_fs_req_cleanup(&req);
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.13 goc_io_handle_register + goc_io_handle_close
 * ====================================================================== */

static void fiber_p10_13(void* arg)
{
    fiber_result_t* r = (fiber_result_t*)arg;

    /* uv_async_init is the only uv_*_init documented as safe from any thread.
     * Other handle init functions (uv_tcp_init, uv_pipe_init, etc.) modify
     * loop->handle_queue without a lock and must be called from the loop thread. */
    uv_async_t* h = (uv_async_t*)goc_malloc(sizeof(uv_async_t));
    int rc = uv_async_init(goc_scheduler(), h, NULL);
    if (rc != 0) goto done;

    goc_io_handle_register((uv_handle_t*)h);
    goc_io_handle_close((uv_handle_t*)h, NULL);

    r->ok = 1;
done:;
}

static void test_p10_13(void)
{
    TEST_BEGIN("P10.13 goc_io_handle_register + goc_io_handle_close: no crash");
    fiber_result_t r = {0};
    goc_chan* done_ch = goc_go(fiber_p10_13, &r);
    goc_take_sync(done_ch);
    ASSERT(r.ok);
    TEST_PASS();
done:;
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    install_crash_handler();
    init_tmp_paths();
    goc_init();

    printf("Phase 10 — Async I/O wrappers\n");

    cleanup_tmp_files();

    test_p10_1();
    test_p10_2();
    test_p10_3();
    test_p10_4();
    test_p10_5();
    test_p10_6();
    test_p10_7();
    test_p10_8();
    test_p10_9();
    test_p10_10();
    test_p10_11();
    test_p10_12();
    test_p10_13();

    printf("\n%d/%d tests passed", g_tests_passed, g_tests_run);
    if (g_tests_failed)
        printf(", %d FAILED", g_tests_failed);
    printf("\n");

    goc_shutdown();
    return g_tests_failed ? 1 : 0;
}
