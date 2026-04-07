/* src/goc_debug.c
 * Buffered debug logging for LIBGOC_DEBUG builds.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <uv.h>

#include "goc.h"

#if defined(_WIN32)
#  include <io.h>
#  define GOC_DBG_WRITE _write
#  define GOC_DBG_STDERR_FILENO _fileno(stderr)
#else
#  include <unistd.h>
#  define GOC_DBG_WRITE write
#  define GOC_DBG_STDERR_FILENO STDERR_FILENO
#endif

#if defined(LIBGOC_DEBUG)

#define GOC_DBG_BUFFER_CAPACITY (16 * 1024)

static char goc_dbg_buffer[GOC_DBG_BUFFER_CAPACITY];
static size_t goc_dbg_buffer_len;
static volatile sig_atomic_t goc_dbg_signal_len;
static bool goc_dbg_enabled;
static uv_mutex_t goc_dbg_mutex;
static uv_once_t goc_dbg_init_once = UV_ONCE_INIT;

static void goc_dbg_init_once_fn(void) {
    uv_mutex_init(&goc_dbg_mutex);
    goc_dbg_buffer_len = 0;
    goc_dbg_signal_len = 0;
    goc_dbg_enabled = false;
}

static void goc_dbg_flush_locked(void);

void goc_dbg_start(void) {
    uv_once(&goc_dbg_init_once, goc_dbg_init_once_fn);
    uv_mutex_lock(&goc_dbg_mutex);
    goc_dbg_enabled = true;
    uv_mutex_unlock(&goc_dbg_mutex);
}

void goc_dbg_stop(void) {
    uv_once(&goc_dbg_init_once, goc_dbg_init_once_fn);
    uv_mutex_lock(&goc_dbg_mutex);
    goc_dbg_flush_locked();
    goc_dbg_enabled = false;
    uv_mutex_unlock(&goc_dbg_mutex);
}

static void goc_dbg_flush_locked(void) {
    if (goc_dbg_buffer_len == 0) {
        return;
    }

    size_t len = goc_dbg_buffer_len;
    size_t wrote = fwrite(goc_dbg_buffer, 1, len, stderr);
    (void)wrote;
    fflush(stderr);
    goc_dbg_buffer_len = 0;
    goc_dbg_signal_len = 0;
}

static size_t goc_dbg_format_timestamp(char *buf, size_t len) {
    if (len == 0) {
        return 0;
    }

    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) {
        return 0;
    }

    struct tm tm_info;
#if defined(_WIN32)
    if (localtime_s(&tm_info, &ts.tv_sec) != 0) {
        return 0;
    }
#else
    if (!localtime_r(&ts.tv_sec, &tm_info)) {
        return 0;
    }
#endif

    int milliseconds = (int)(ts.tv_nsec / 1000000);
    int written = snprintf(buf, len,
                           "%04d:%02d:%02d:%02d:%02d:%02d:%03d",
                           tm_info.tm_year + 1900,
                           tm_info.tm_mon + 1,
                           tm_info.tm_mday,
                           tm_info.tm_hour,
                           tm_info.tm_min,
                           tm_info.tm_sec,
                           milliseconds);
    if (written < 0 || (size_t)written >= len) {
        return 0;
    }

    return (size_t)written;
}

void goc_dbg_log(const char *fmt, ...) {
    uv_once(&goc_dbg_init_once, goc_dbg_init_once_fn);

    uv_mutex_lock(&goc_dbg_mutex);
    if (!goc_dbg_enabled) {
        uv_mutex_unlock(&goc_dbg_mutex);
        return;
    }
    uv_mutex_unlock(&goc_dbg_mutex);

    const char *message_fmt = fmt;
    size_t prefix_len = 0;
    static const char prefix[] = "[GOC_DBG] ";
    static const size_t prefix_size = sizeof(prefix) - 1;

    if (strncmp(fmt, prefix, prefix_size) == 0) {
        message_fmt += prefix_size;
        prefix_len = prefix_size;
    }

    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, message_fmt, ap);
    va_end(ap);
    if (needed < 0) {
        return;
    }

    size_t user_msg_len = (size_t)needed;
    char user_small_buf[512];
    char *user_msg = user_small_buf;
    bool user_allocated = false;

    if (user_msg_len >= sizeof(user_small_buf)) {
        user_msg = malloc(user_msg_len + 1);
        if (!user_msg) {
            return;
        }
        user_allocated = true;
    }

    va_start(ap, fmt);
    int result = vsnprintf(user_msg, user_allocated ? user_msg_len + 1 : sizeof(user_small_buf), message_fmt, ap);
    va_end(ap);
    if (result < 0) {
        if (user_allocated) {
            free(user_msg);
        }
        return;
    }

    size_t actual_user_len = (size_t)result;
    char time_buf[32];
    size_t time_len = goc_dbg_format_timestamp(time_buf, sizeof(time_buf));

    size_t final_len = prefix_len + 3 + time_len + actual_user_len;
    char msg_small_buf[512];
    char *msg = msg_small_buf;
    bool allocated = false;

    if (final_len >= sizeof(msg_small_buf)) {
        msg = malloc(final_len + 1);
        if (!msg) {
            if (user_allocated) {
                free(user_msg);
            }
            return;
        }
        allocated = true;
    }

    size_t offset = 0;
    if (prefix_len > 0) {
        memcpy(msg + offset, prefix, prefix_len);
        offset += prefix_len;
    }

    if (time_len > 0) {
        msg[offset++] = '[';
        memcpy(msg + offset, time_buf, time_len);
        offset += time_len;
        msg[offset++] = ']';
        msg[offset++] = ' ';
    }

    memcpy(msg + offset, user_msg, actual_user_len);
    offset += actual_user_len;
    msg[offset] = '\0';
    size_t actual_len = offset;

    if (user_allocated) {
        free(user_msg);
    }

    uv_mutex_lock(&goc_dbg_mutex);
    if (!goc_dbg_enabled) {
        uv_mutex_unlock(&goc_dbg_mutex);
        if (allocated) {
            free(msg);
        }
        return;
    }

    if (actual_len >= GOC_DBG_BUFFER_CAPACITY) {
        goc_dbg_flush_locked();
        fwrite(msg, 1, actual_len, stderr);
        fflush(stderr);
    } else {
        if (goc_dbg_buffer_len + actual_len > GOC_DBG_BUFFER_CAPACITY) {
            goc_dbg_flush_locked();
        }

        memcpy(goc_dbg_buffer + goc_dbg_buffer_len, msg, actual_len);
        goc_dbg_buffer_len += actual_len;
        goc_dbg_signal_len = (sig_atomic_t)goc_dbg_buffer_len;
    }

    uv_mutex_unlock(&goc_dbg_mutex);

    if (allocated) {
        free(msg);
    }
}

void goc_dbg_flush(void) {
    uv_once(&goc_dbg_init_once, goc_dbg_init_once_fn);
    uv_mutex_lock(&goc_dbg_mutex);
    goc_dbg_flush_locked();
    uv_mutex_unlock(&goc_dbg_mutex);
}

void goc_dbg_flush_signal_safe(void) {
    sig_atomic_t len = goc_dbg_signal_len;
    if (len <= 0) {
        return;
    }

    size_t remaining = (size_t)len;
    const char *data = goc_dbg_buffer;

    while (remaining > 0) {
        ssize_t rc = GOC_DBG_WRITE(GOC_DBG_STDERR_FILENO, data, remaining);
        if (rc <= 0) {
            break;
        }
        data += (size_t)rc;
        remaining -= (size_t)rc;
    }
}

#endif /* LIBGOC_DEBUG */
