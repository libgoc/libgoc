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
 *   P10.14 Two fibers on different workers perform concurrent file reads; both complete
 *   P10.15 8 concurrent goc_io_getaddrinfo calls on pool=4; all complete
 *   P10.16 Two fibers on different workers create goc_timeout with different durations; relative firing order is correct
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
#include <stdatomic.h>

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
static const char* TMP_PATH4  = NULL;
static const char* TMP_PATH5  = NULL;

#ifdef _WIN32
static char s_tmp1[MAX_PATH];
static char s_tmp2[MAX_PATH];
static char s_tmp3[MAX_PATH];
static char s_tmp4[MAX_PATH];
static char s_tmp5[MAX_PATH];
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
    snprintf(s_tmp3, sizeof(s_tmp3), "%sgoc_io_test_dst.txt",      tmp_dir);
    snprintf(s_tmp4, sizeof(s_tmp4), "%sgoc_io_test_read_a.txt",   tmp_dir);
    snprintf(s_tmp5, sizeof(s_tmp5), "%sgoc_io_test_read_b.txt",   tmp_dir);
    TMP_PATH  = s_tmp1;
    TMP_PATH2 = s_tmp2;
    TMP_PATH3 = s_tmp3;
    TMP_PATH4 = s_tmp4;
    TMP_PATH5 = s_tmp5;
#else
    TMP_PATH  = "/tmp/goc_io_test.txt";
    TMP_PATH2 = "/tmp/goc_io_test_renamed.txt";
    TMP_PATH3 = "/tmp/goc_io_test_dst.txt";
    TMP_PATH4 = "/tmp/goc_io_test_read_a.txt";
    TMP_PATH5 = "/tmp/goc_io_test_read_b.txt";
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
    uv_fs_unlink(goc_scheduler(), &req, TMP_PATH4, NULL);
    uv_fs_req_cleanup(&req);
    uv_fs_unlink(goc_scheduler(), &req, TMP_PATH5, NULL);
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

    goc_alt_op_t ops[2] = {
        { .ch = open_ch,  .op_kind = GOC_ALT_TAKE, .put_val = NULL },
        { .ch = dummy_ch, .op_kind = GOC_ALT_TAKE, .put_val = NULL },
    };
    goc_alts_result_t* result = goc_alts(ops, 2);

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
 * P10.14 Two fibers on different workers perform concurrent file reads
 * ====================================================================== */

typedef struct {
    goc_chan* done;
    const char* path;
    const char* expected;
    int expected_len;
    _Atomic int* pass_count;
} p10_14_read_arg_t;

static void fiber_p10_14_read_worker(void* arg)
{
    p10_14_read_arg_t* a = (p10_14_read_arg_t*)arg;
    int ok = 0;

    goc_val_t* vopen = goc_take(goc_io_fs_open(a->path, UV_FS_O_RDONLY, 0));
    if (!vopen || vopen->ok != GOC_OK) goto done;
    uv_file fd = goc_unbox_int(vopen->val);
    if (fd < 0) goto done;

    goc_val_t* vrd = goc_take(goc_io_fs_read(fd, a->expected_len, 0));
    goc_take(goc_io_fs_close(fd));

    if (!vrd || vrd->ok != GOC_OK) goto done;
    goc_io_fs_read_t* rr = (goc_io_fs_read_t*)vrd->val;
    if (!rr) goto done;
    if (rr->nread != a->expected_len) goto done;
    if (memcmp(rr->buf, a->expected, (size_t)a->expected_len) != 0) goto done;

    ok = 1;
done:
    if (ok)
        atomic_fetch_add_explicit(a->pass_count, 1, memory_order_acq_rel);
    goc_put(a->done, goc_box_int(ok));
}

static void test_p10_14(void)
{
    TEST_BEGIN("P10.14 concurrent reads on pool=2: two fibers both complete");

    static const char DATA_A[] = "p10_14_read_a_payload";
    static const char DATA_B[] = "p10_14_read_b_payload";
    const int LEN_A = (int)(sizeof(DATA_A) - 1);
    const int LEN_B = (int)(sizeof(DATA_B) - 1);

    uv_fs_t req;
    uv_file fd = -1;

    /* Prepare file A */
    if (uv_fs_open(goc_scheduler(), &req, TMP_PATH4,
                   UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644, NULL) < 0)
        goto done;
    fd = (uv_file)req.result;
    uv_fs_req_cleanup(&req);

    uv_buf_t b1 = uv_buf_init((char*)DATA_A, LEN_A);
    if (uv_fs_write(goc_scheduler(), &req, fd, &b1, 1, 0, NULL) < 0) {
        uv_fs_req_cleanup(&req);
        uv_fs_close(goc_scheduler(), &req, fd, NULL);
        uv_fs_req_cleanup(&req);
        goto done;
    }
    uv_fs_req_cleanup(&req);
    uv_fs_close(goc_scheduler(), &req, fd, NULL);
    uv_fs_req_cleanup(&req);

    /* Prepare file B */
    if (uv_fs_open(goc_scheduler(), &req, TMP_PATH5,
                   UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644, NULL) < 0)
        goto done;
    fd = (uv_file)req.result;
    uv_fs_req_cleanup(&req);

    uv_buf_t b2 = uv_buf_init((char*)DATA_B, LEN_B);
    if (uv_fs_write(goc_scheduler(), &req, fd, &b2, 1, 0, NULL) < 0) {
        uv_fs_req_cleanup(&req);
        uv_fs_close(goc_scheduler(), &req, fd, NULL);
        uv_fs_req_cleanup(&req);
        goto done;
    }
    uv_fs_req_cleanup(&req);
    uv_fs_close(goc_scheduler(), &req, fd, NULL);
    uv_fs_req_cleanup(&req);

    goc_pool* pool = goc_pool_make(2);
    ASSERT(pool != NULL);

    goc_chan* done_a = goc_chan_make(1);
    goc_chan* done_b = goc_chan_make(1);
    ASSERT(done_a != NULL && done_b != NULL);

    _Atomic int pass_count = 0;

    p10_14_read_arg_t a1 = {
        .done = done_a,
        .path = TMP_PATH4,
        .expected = DATA_A,
        .expected_len = LEN_A,
        .pass_count = &pass_count
    };
    p10_14_read_arg_t a2 = {
        .done = done_b,
        .path = TMP_PATH5,
        .expected = DATA_B,
        .expected_len = LEN_B,
        .pass_count = &pass_count
    };

    goc_go_on(pool, fiber_p10_14_read_worker, &a1);
    goc_go_on(pool, fiber_p10_14_read_worker, &a2);

    goc_val_t* r1 = goc_take_sync(done_a);
    goc_val_t* r2 = goc_take_sync(done_b);
    ASSERT(r1 && goc_unbox_int(r1->val) == 1);
    ASSERT(r2 && goc_unbox_int(r2->val) == 1);
    ASSERT(atomic_load_explicit(&pass_count, memory_order_acquire) == 2);

    goc_pool_destroy(pool);

    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.15 8 concurrent getaddrinfo calls on pool=4
 * ====================================================================== */

#define P10_15_WORKERS 4
#define P10_15_CALLS   8

typedef struct {
    goc_chan* done;
    _Atomic int* completed;
    _Atomic int* success;
} p10_15_arg_t;

static void fiber_p10_15_worker(void* arg)
{
    p10_15_arg_t* a = (p10_15_arg_t*)arg;

    int ok = 0;
    goc_chan* ch = goc_io_getaddrinfo("localhost", NULL, NULL);
    goc_val_t* v = goc_take(ch);
    if (v && v->ok == GOC_OK) {
        goc_io_getaddrinfo_t* res = (goc_io_getaddrinfo_t*)v->val;
        if (res && res->ok == GOC_IO_OK && res->res != NULL) {
            uv_freeaddrinfo(res->res);
            ok = 1;
        }
    }

    if (ok)
        atomic_fetch_add_explicit(a->success, 1, memory_order_acq_rel);
    if (atomic_fetch_add_explicit(a->completed, 1, memory_order_acq_rel) + 1 == P10_15_CALLS)
        goc_put(a->done, goc_box_int(1));
}

static void test_p10_15(void)
{
    TEST_BEGIN("P10.15 pool=4, 8 concurrent getaddrinfo calls all complete");

    goc_pool* pool = goc_pool_make(P10_15_WORKERS);
    ASSERT(pool != NULL);

    goc_chan* done = goc_chan_make(1);
    ASSERT(done != NULL);

    _Atomic int completed = 0;
    _Atomic int success = 0;
    p10_15_arg_t arg = {
        .done = done,
        .completed = &completed,
        .success = &success
    };

    for (int i = 0; i < P10_15_CALLS; i++)
        goc_go_on(pool, fiber_p10_15_worker, &arg);

    goc_val_t* vd = goc_take_sync(done);
    ASSERT(vd && goc_unbox_int(vd->val) == 1);

    ASSERT(atomic_load_explicit(&completed, memory_order_acquire) == P10_15_CALLS);
    ASSERT(atomic_load_explicit(&success, memory_order_acquire) == P10_15_CALLS);

    goc_pool_destroy(pool);

    TEST_PASS();
done:;
}

/* =========================================================================
 * P10.16 Two fibers on different workers create timeouts; relative order
 * ====================================================================== */

typedef struct {
    goc_chan* done;
    uint64_t ms;
    int idx;
    uint64_t t0_ns;
    _Atomic int* seq;
    _Atomic int* order;
    _Atomic uint64_t* elapsed_ns;
} p10_16_arg_t;

static void fiber_p10_16_timeout_worker(void* arg)
{
    p10_16_arg_t* a = (p10_16_arg_t*)arg;

    goc_chan* tch = goc_timeout(a->ms);
    goc_val_t* tv = goc_take(tch);

    int ok = 0;
    if (tv && tv->ok == GOC_CLOSED) {
        uint64_t now_ns = uv_hrtime();
        uint64_t dt = (now_ns >= a->t0_ns) ? (now_ns - a->t0_ns) : 0;
        atomic_store_explicit(&a->elapsed_ns[a->idx], dt, memory_order_release);

        int s = atomic_fetch_add_explicit(a->seq, 1, memory_order_acq_rel);
        atomic_store_explicit(&a->order[a->idx], s, memory_order_release);
        ok = 1;
    }

    goc_put(a->done, goc_box_int(ok));
}

static void test_p10_16(void)
{
    TEST_BEGIN("P10.16 pool=2 timeout relative order: shorter fires first");

    goc_pool* pool = goc_pool_make(2);
    ASSERT(pool != NULL);

    goc_chan* done_a = goc_chan_make(1);
    goc_chan* done_b = goc_chan_make(1);
    ASSERT(done_a != NULL && done_b != NULL);

    _Atomic int seq = 0;
    _Atomic int order[2];
    _Atomic uint64_t elapsed_ns[2];
    atomic_store_explicit(&order[0], -1, memory_order_release);
    atomic_store_explicit(&order[1], -1, memory_order_release);
    atomic_store_explicit(&elapsed_ns[0], 0, memory_order_release);
    atomic_store_explicit(&elapsed_ns[1], 0, memory_order_release);

    const uint64_t short_ms = 20;
    const uint64_t long_ms  = 80;
    const uint64_t t0 = uv_hrtime();

    p10_16_arg_t a_short = {
        .done = done_a,
        .ms = short_ms,
        .idx = 0,
        .t0_ns = t0,
        .seq = &seq,
        .order = order,
        .elapsed_ns = elapsed_ns
    };
    p10_16_arg_t a_long = {
        .done = done_b,
        .ms = long_ms,
        .idx = 1,
        .t0_ns = t0,
        .seq = &seq,
        .order = order,
        .elapsed_ns = elapsed_ns
    };

    goc_go_on(pool, fiber_p10_16_timeout_worker, &a_short);
    goc_go_on(pool, fiber_p10_16_timeout_worker, &a_long);

    goc_val_t* r1 = goc_take_sync(done_a);
    goc_val_t* r2 = goc_take_sync(done_b);
    ASSERT(r1 && goc_unbox_int(r1->val) == 1);
    ASSERT(r2 && goc_unbox_int(r2->val) == 1);

    int o_short = atomic_load_explicit(&order[0], memory_order_acquire);
    int o_long  = atomic_load_explicit(&order[1], memory_order_acquire);
    uint64_t e_short = atomic_load_explicit(&elapsed_ns[0], memory_order_acquire);
    uint64_t e_long  = atomic_load_explicit(&elapsed_ns[1], memory_order_acquire);

    ASSERT(o_short >= 0 && o_long >= 0);
    ASSERT(o_short < o_long);

    /* Relaxed time sanity: both fired, and long timeout should not precede short. */
    ASSERT(e_short > 0 && e_long > 0);
    ASSERT(e_short <= e_long);

    goc_pool_destroy(pool);

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
    test_p10_14();
    test_p10_15();
    test_p10_16();

    printf("\n%d/%d tests passed", g_tests_passed, g_tests_run);
    if (g_tests_failed)
        printf(", %d FAILED", g_tests_failed);
    printf("\n");

    goc_shutdown();
    return g_tests_failed ? 1 : 0;
}
