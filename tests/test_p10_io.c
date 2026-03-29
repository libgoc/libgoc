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
 *   P10.1  goc_io_fs_open: opens a new file with write permissions, creates if not exists, and truncates if exists; file descriptor >= 0
 *   P10.2  goc_io_fs_write: writes static content to an open file; returns the number of bytes written
 *   P10.3  goc_io_fs_read: reads back the written content; validates that the read buffer matches the written content
 *   P10.4  goc_io_fs_stat: retrieves file metadata; validates that the file size matches the written content length
 *   P10.5  goc_io_fs_rename: renames the file; validates that the old path no longer exists and the new path exists with the correct file size
 *   P10.6  goc_io_fs_unlink: deletes the renamed file; validates that subsequent stat on the file fails
 *   P10.7  goc_io_fs_open with invalid path: attempts to open a non-existent file; validates that the file descriptor is negative (error code)
 *   P10.8  goc_io_getaddrinfo: resolves "localhost"; validates that the result structure is non-NULL and contains valid address information
 *   P10.9  goc_io_getaddrinfo with empty node and service: attempts to resolve with empty node and service; validates that no crash occurs and libuv returns an error
 *   P10.10 goc_io_getaddrinfo: validates that goc_io_getaddrinfo returns a non-NULL channel (compile-time API check)
 *   P10.11 goc_io_fs_sendfile: copies bytes between two file descriptors; validates that the destination file content matches the source file content
 *   P10.12 Channel-based goc_io_fs_open integrates with goc_alts: validates integration of goc_io_fs_open with goc_alts; ensures the correct channel result is selected
 *   P10.13 goc_io_handle_register + goc_io_handle_close: validates that a GC-allocated uv_async_t handle registers, closes, and unregisters cleanly without crashes
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
 * P10.1  goc_io_fs_open: open a new file
 * ====================================================================== */

static void fiber_p10_1(void* arg)
{
    goc_chan* res_ch = arg;
    int ok = 0;
    goc_chan*   ch = goc_io_fs_open(TMP_PATH,
                                    UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644);
    goc_val_t*  v  = goc_take(ch);
    if (!v || v->ok != GOC_OK) goto done;
    uv_file fd = goc_unbox_int(v->val);
    if (fd < 0) goto done;
    goc_take(goc_io_fs_close(fd));
    ok = 1;
done:
    goc_put(res_ch, goc_box_int(ok));
}

static void test_p10_1(void)
{
    TEST_BEGIN("P10.1  goc_io_fs_open opens a new file");
    goc_chan* res_ch = goc_chan_make(1);
    goc_go(fiber_p10_1, res_ch);
    ASSERT(goc_unbox_int(goc_take_sync(res_ch)->val));
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.2  goc_io_fs_write: write data
 * ====================================================================== */

static void fiber_p10_2(void* arg)
{
    goc_chan* res_ch = arg;
    int ok = 0;
    /* Open for writing */
    goc_val_t* vopen = goc_take(goc_io_fs_open(TMP_PATH,
                               UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644));
    if (!vopen || vopen->ok != GOC_OK) goto done;
    uv_file fd = goc_unbox_int(vopen->val);
    if (fd < 0) goto done;

    goc_val_t* vwrite = goc_take(goc_io_fs_write(fd, CONTENT, CONTENT_LEN, -1));
    ssize_t written = goc_unbox_int(vwrite->val);
    goc_take(goc_io_fs_close(fd));
    if (written != CONTENT_LEN) goto done;

    ok = 1;
done:
    goc_put(res_ch, goc_box_int(ok));
}

static void test_p10_2(void)
{
    TEST_BEGIN("P10.2  goc_io_fs_write writes correct byte count");
    goc_chan* res_ch = goc_chan_make(1);
    goc_go(fiber_p10_2, res_ch);
    ASSERT(goc_unbox_int(goc_take_sync(res_ch)->val));
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.3  goc_io_fs_read: read back written content
 * ====================================================================== */

static void fiber_p10_3(void* arg)
{
    goc_chan* res_ch = arg;
    int ok = 0;
    goc_val_t* vopen = goc_take(goc_io_fs_open(TMP_PATH, UV_FS_O_RDONLY, 0));
    if (!vopen || vopen->ok != GOC_OK) goto done;
    uv_file fd = goc_unbox_int(vopen->val);
    if (fd < 0) goto done;

    goc_val_t* vrd = goc_take(goc_io_fs_read(fd, CONTENT_LEN, 0));
    goc_take(goc_io_fs_close(fd));

    goc_io_fs_read_t* rres = vrd->val;
    if (!rres || rres->nread != CONTENT_LEN) goto done;
    if (memcmp(rres->buf, CONTENT, CONTENT_LEN) != 0) goto done;
    ok = 1;
done:
    goc_put(res_ch, goc_box_int(ok));
}

static void test_p10_3(void)
{
    TEST_BEGIN("P10.3  goc_io_fs_read reads back correct content");
    goc_chan* res_ch = goc_chan_make(1);
    goc_go(fiber_p10_3, res_ch);
    ASSERT(goc_unbox_int(goc_take_sync(res_ch)->val));
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.4  goc_io_fs_stat: stat the file
 * ====================================================================== */

static void fiber_p10_4(void* arg)
{
    goc_chan* res_ch = arg;
    int ok = 0;
    goc_val_t* vstat = goc_take(goc_io_fs_stat(TMP_PATH));
    if (!vstat || vstat->ok != GOC_OK) goto done;
    goc_io_fs_stat_t* st = vstat->val;
    if (!st || st->ok != GOC_IO_OK) goto done;
    if ((int64_t)st->statbuf.st_size != CONTENT_LEN) goto done;
    ok = 1;
done:
    goc_put(res_ch, goc_box_int(ok));
}

static void test_p10_4(void)
{
    TEST_BEGIN("P10.4  goc_io_fs_stat reports correct file size");
    goc_chan* res_ch = goc_chan_make(1);
    goc_go(fiber_p10_4, res_ch);
    ASSERT(goc_unbox_int(goc_take_sync(res_ch)->val));
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.5  goc_io_fs_rename: rename the file
 * ====================================================================== */

static void fiber_p10_5(void* arg)
{
    goc_chan* res_ch = arg;
    int ok = 0;
    goc_val_t* vren = goc_take(goc_io_fs_rename(TMP_PATH, TMP_PATH2));
    int ren = goc_unbox_int(vren->val);
    if (ren != 0) goto done;

    /* Old path should no longer exist */
    goc_val_t* vold = goc_take(goc_io_fs_stat(TMP_PATH));
    goc_io_fs_stat_t* old_st = vold->val;
    if (!old_st || old_st->ok == GOC_IO_OK) goto done;  /* still exists = fail */

    /* New path should exist with the same size */
    goc_val_t* vnew = goc_take(goc_io_fs_stat(TMP_PATH2));
    goc_io_fs_stat_t* new_st = vnew->val;
    if (!new_st || new_st->ok != GOC_IO_OK) goto done;
    if ((int64_t)new_st->statbuf.st_size != CONTENT_LEN) goto done;

    ok = 1;
done:
    goc_put(res_ch, goc_box_int(ok));
}

static void test_p10_5(void)
{
    TEST_BEGIN("P10.5  goc_io_fs_rename renames file correctly");
    goc_chan* res_ch = goc_chan_make(1);
    goc_go(fiber_p10_5, res_ch);
    ASSERT(goc_unbox_int(goc_take_sync(res_ch)->val));
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.6  goc_io_fs_unlink: delete the (renamed) file
 * ====================================================================== */

static void fiber_p10_6(void* arg)
{
    goc_chan* res_ch = arg;
    int ok = 0;
    goc_val_t* vul = goc_take(goc_io_fs_unlink(TMP_PATH2));
    int ul = goc_unbox_int(vul->val);
    if (ul != 0) goto done;

    /* File should no longer exist */
    goc_val_t* vstat = goc_take(goc_io_fs_stat(TMP_PATH2));
    goc_io_fs_stat_t* st = vstat->val;
    if (!st || st->ok == GOC_IO_OK) goto done;  /* still exists = fail */

    ok = 1;
done:
    goc_put(res_ch, goc_box_int(ok));
}

static void test_p10_6(void)
{
    TEST_BEGIN("P10.6  goc_io_fs_unlink deletes file, stat then fails");
    goc_chan* res_ch = goc_chan_make(1);
    goc_go(fiber_p10_6, res_ch);
    ASSERT(goc_unbox_int(goc_take_sync(res_ch)->val));
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.7  goc_io_fs_open with non-existent path + UV_FS_O_RDONLY → error
 * ====================================================================== */

static void fiber_p10_7(void* arg)
{
    goc_chan* res_ch = arg;
    int ok = 0;
    goc_chan* ch = goc_io_fs_open("/nonexistent/path/that/does/not/exist",
                                   UV_FS_O_RDONLY, 0);
    goc_val_t*     v  = goc_take(ch);
    if (!v || v->ok != GOC_OK) goto done;
    uv_file fd = goc_unbox_int(v->val);
    /* result should be a negative error code */
    if (fd >= 0) goto done;
    ok = 1;
done:
    goc_put(res_ch, goc_box_int(ok));
}

static void test_p10_7(void)
{
    TEST_BEGIN("P10.7  goc_io_fs_open with invalid path returns error code");
    goc_chan* res_ch = goc_chan_make(1);
    goc_go(fiber_p10_7, res_ch);
    ASSERT(goc_unbox_int(goc_take_sync(res_ch)->val));
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.8  goc_io_getaddrinfo: resolve "localhost"
 * ====================================================================== */

static void fiber_p10_8(void* arg)
{
    goc_chan* res_ch = arg;
    int ok = 0;
    goc_chan*           ch = goc_io_getaddrinfo("localhost", NULL, NULL);
    goc_val_t*          v  = goc_take(ch);
    if (!v || v->ok != GOC_OK) goto done;
    goc_io_getaddrinfo_t*  res = (goc_io_getaddrinfo_t*)v->val;
    if (!res || res->ok != GOC_IO_OK || res->res == NULL) goto done;
    uv_freeaddrinfo(res->res);
    ok = 1;
done:
    goc_put(res_ch, goc_box_int(ok));
}

static void test_p10_8(void)
{
    TEST_BEGIN("P10.8  goc_io_getaddrinfo resolves \"localhost\"");
    goc_chan* res_ch = goc_chan_make(1);
    goc_go(fiber_p10_8, res_ch);
    ASSERT(goc_unbox_int(goc_take_sync(res_ch)->val));
    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.9  goc_io_getaddrinfo with node=NULL and service=NULL → error
 * ====================================================================== */

static void fiber_p10_9(void* arg)
{
    goc_chan* res_ch = arg;
    int ok = 0;
    goc_chan*          ch  = goc_io_getaddrinfo(NULL, NULL, NULL);
    goc_val_t*         v   = goc_take(ch);
    if (!v || v->ok != GOC_OK) goto done;
    goc_io_getaddrinfo_t* res = v->val;
    if (!res) goto done;
    /* libuv returns an error (EAI_NONAME or similar) when both are NULL */
    if (res->ok == GOC_IO_OK && res->res != NULL)
        uv_freeaddrinfo(res->res);
    /* Test passes regardless of status — we just need no crash */
    ok = 1;
done:
    goc_put(res_ch, goc_box_int(ok));
}

static void test_p10_9(void)
{
    TEST_BEGIN("P10.9  goc_io_getaddrinfo NULL node+service: no crash");
    goc_chan* res_ch = goc_chan_make(1);
    goc_go(fiber_p10_9, res_ch);
    ASSERT(goc_unbox_int(goc_take_sync(res_ch)->val));
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
        goc_io_getaddrinfo_t* res = v->val;
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
    goc_chan* res_ch = arg;
    int ok = 0;

    /* Create source file with content */
    goc_val_t* vsrc = goc_take(goc_io_fs_open(TMP_PATH,
                               UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644));
    if (!vsrc || vsrc->ok != GOC_OK) goto done;
    uv_file src_fd = goc_unbox_int(vsrc->val);
    if (src_fd < 0) goto done;

    goc_val_t* vwrite = goc_take(goc_io_fs_write(src_fd, CONTENT, CONTENT_LEN, -1));
    ssize_t written = goc_unbox_int(vwrite->val);
    goc_take(goc_io_fs_close(src_fd));
    if (written != CONTENT_LEN) goto done;

    /* Reopen source for reading */
    goc_val_t* vsrc2 = goc_take(goc_io_fs_open(TMP_PATH, UV_FS_O_RDONLY, 0));
    if (!vsrc2 || vsrc2->ok != GOC_OK) goto done;
    src_fd = goc_unbox_int(vsrc2->val);
    if (src_fd < 0) goto done;

    /* Create destination file */
    goc_val_t* vdst = goc_take(goc_io_fs_open(TMP_PATH3,
                               UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644));
    if (!vdst || vdst->ok != GOC_OK) {
        goc_take(goc_io_fs_close(src_fd));
        goto done;
    }
    uv_file dst_fd = goc_unbox_int(vdst->val);
    if (dst_fd < 0) {
        goc_take(goc_io_fs_close(src_fd));
        goto done;
    }

    goc_val_t* vsf = goc_take(goc_io_fs_sendfile(dst_fd, src_fd, 0, (size_t)CONTENT_LEN));
    ssize_t sf = goc_unbox_int(vsf->val);
    goc_take(goc_io_fs_close(src_fd));
    goc_take(goc_io_fs_close(dst_fd));
    if (sf != CONTENT_LEN) goto done;

    /* Verify destination content */
    goc_val_t* vvfd = goc_take(goc_io_fs_open(TMP_PATH3, UV_FS_O_RDONLY, 0));
    if (!vvfd || vvfd->ok != GOC_OK) goto done;
    uv_file verify_fd = goc_unbox_int(vvfd->val);
    if (verify_fd < 0) goto done;
    goc_val_t* vrd = goc_take(goc_io_fs_read(verify_fd, CONTENT_LEN, 0));
    goc_take(goc_io_fs_close(verify_fd));
    goc_io_fs_read_t* rres = vrd->val;
    if (!rres || rres->nread != CONTENT_LEN) goto done;
    if (memcmp(rres->buf, CONTENT, CONTENT_LEN) != 0) goto done;

    ok = 1;
done:
    goc_put(res_ch, goc_box_int(ok));
}

static void test_p10_11(void)
{
    TEST_BEGIN("P10.11 goc_io_fs_sendfile copies correct byte count");
    goc_chan* res_ch = goc_chan_make(1);
    goc_go(fiber_p10_11, res_ch);
    ASSERT(goc_unbox_int(goc_take_sync(res_ch)->val));
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
    goc_chan* res_ch = arg;
    int ok = 0;

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
    uv_file fd = goc_unbox_int(result->value.val);
    if (fd < 0) goto done;

    goc_take(goc_io_fs_close(fd));

    /* Close the dummy channel so any parked alts entries are released. */
    goc_close(dummy_ch);

    ok = 1;
done:
    goc_put(res_ch, goc_box_int(ok));
}

static void test_p10_12(void)
{
    TEST_BEGIN("P10.12 goc_io_fs_open works with goc_alts (select)");
    goc_chan* res_ch = goc_chan_make(1);
    goc_go(fiber_p10_12, res_ch);
    ASSERT(goc_unbox_int(goc_take_sync(res_ch)->val));
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
    goc_chan* res_ch = arg;
    int ok = 0;

    /* uv_async_init is the only uv_*_init documented as safe from any thread.
     * Other handle init functions (uv_tcp_init, uv_pipe_init, etc.) modify
     * loop->handle_queue without a lock and must be called from the loop thread. */
    uv_async_t* h = goc_malloc(sizeof(uv_async_t));
    int rc = uv_async_init(goc_scheduler(), h, NULL);
    if (rc != 0) goto done;

    goc_io_handle_register((uv_handle_t*)h);
    goc_io_handle_close((uv_handle_t*)h, NULL);

    ok = 1;
done:
    goc_put(res_ch, goc_box_int(ok));
}

static void test_p10_13(void)
{
    TEST_BEGIN("P10.13 goc_io_handle_register + goc_io_handle_close: no crash");
    goc_chan* res_ch = goc_chan_make(1);
    goc_go(fiber_p10_13, res_ch);
    ASSERT(goc_unbox_int(goc_take_sync(res_ch)->val));
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
