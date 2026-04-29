/**
 * @file log_buffer.c
 * @brief PSRAM ring buffer for runtime log capture.
 *
 * Allocates 80% of free PSRAM as a circular buffer of fixed-size log_entry_t.
 * Hooks into ESP-IDF's logging via esp_log_set_vprintf().
 *
 * Thread safety: the ring buffer uses a single-writer model (vprintf hook
 * always runs in the logging task's context). Readers (HTTP handler) use
 * a spinlock for safe access.
 */
#include "log_buffer.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define TAG "logbuf"

/* ─────────────────────────────────────────────────────────────────── */
/* Static ring buffer state                                             */
/* ─────────────────────────────────────────────────────────────────── */
static log_entry_t *s_entries    = NULL;
static int           s_capacity  = 0;       /* total slots */
static int           s_head      = 0;       /* where NEXT entry will be written */
static bool          s_wrapped   = false;   /* buffer has wrapped at least once */
static portMUX_TYPE  s_lock      = portMUX_INITIALIZER_UNLOCKED;

/* Previous vprintf function to chain to (serial output) */
static vprintf_like_t s_orig_vprintf = NULL;

/* ─────────────────────────────────────────────────────────────────── */
/* ESP_LOG vprintf hook                                                  */
/* ─────────────────────────────────────────────────────────────────── */

/**
 * ESP_LOG macros pass a format like:
 *   "I (12345) tag: message\n"   (+ variadic args for the message part)
 *
 * Strategy: format the full message with vsnprintf first, then parse the
 * formatted output to extract level, timestamp, tag, and message text.
 * This avoids va_list alignment issues — we don't need to know how many
 * format specifiers were consumed by the prefix vs. the message body.
 */
static int log_vprintf_hook(const char *fmt, va_list args)
{
    /* ── Pass-through to serial (with va_copy so original args survive) ── */
    va_list args_copy;
    va_copy(args_copy, args);

    int ret = 0;
    if (s_orig_vprintf) {
        ret = s_orig_vprintf(fmt, args_copy);
    }
    va_end(args_copy);

    /* Skip if buffer not initialised */
    if (s_entries == NULL || s_capacity == 0) return ret;

    /* ── Format the entire log line into a temp buffer ──────────────── */
    char formatted[256];
    int full_len = vsnprintf(formatted, sizeof(formatted), fmt, args);
    if (full_len < 0) full_len = 0;
    if (full_len >= (int)sizeof(formatted)) full_len = (int)sizeof(formatted) - 1;

    /* Remove trailing newline */
    if (full_len > 0 && formatted[full_len - 1] == '\n') {
        formatted[full_len - 1] = '\0';
        full_len--;
    }

    /* ── Parse the formatted output: "L (ts) tag: message" ──────────── */
    char level_char = '?';
    char tag_str[LOG_TAG_MAX] = {0};
    const char *msg_start = formatted;

    /* Try to match ESP_LOG pattern: first char is level, then " (" */
    if (full_len >= 4 && formatted[1] == ' ' && formatted[2] == '(') {
        level_char = formatted[0];
        /* Skip "L (timestamp) " — find the closing paren+space */
        const char *p = formatted + 3;
        while (*p && *p != ')') p++;
        if (*p == ')') p++;
        if (*p == ' ') p++;

        /* Extract tag (characters up to ':') */
        const char *tag_begin = p;
        while (*p && *p != ':') p++;
        int tag_len = (int)(p - tag_begin);
        if (tag_len > LOG_TAG_MAX - 1) tag_len = LOG_TAG_MAX - 1;
        if (tag_len > 0) {
            memcpy(tag_str, tag_begin, tag_len);
            tag_str[tag_len] = '\0';
        }

        /* Skip ": " to get to the message */
        if (*p == ':') p++;
        if (*p == ' ') p++;
        msg_start = p;
    } else {
        /* Non-standard format — store entire line as-is */
        level_char = 'I';
    }

    /* ── Write to ring buffer ───────────────────────────────────────── */
    /* If no tag was parsed, append to the previous entry instead of creating a new one */
    if (tag_str[0] == '\0' && msg_start[0] != '\0') {
        portENTER_CRITICAL(&s_lock);
        int prev = (s_head - 1 + s_capacity) % s_capacity;
        log_entry_t *prev_entry = &s_entries[prev];
        int existing = (int)strlen(prev_entry->msg);
        int space = LOG_MSG_MAX - 1 - existing;
        if (space > 1) {
            prev_entry->msg[existing] = ' ';
            int append_len = (int)strlen(msg_start);
            if (append_len > space - 1) append_len = space - 1;
            memcpy(prev_entry->msg + existing + 1, msg_start, append_len);
            prev_entry->msg[existing + 1 + append_len] = '\0';
        }
        portEXIT_CRITICAL(&s_lock);
        return ret;
    }

    portENTER_CRITICAL(&s_lock);
    int idx = s_head;
    s_head = (s_head + 1) % s_capacity;
    if (s_head == 0 || s_wrapped) s_wrapped = true;
    portEXIT_CRITICAL(&s_lock);

    log_entry_t *entry = &s_entries[idx];
    entry->timestamp_ms = (int64_t)esp_timer_get_time() / 1000LL;
    entry->level        = (uint8_t)level_char;
    strncpy(entry->tag, tag_str, LOG_TAG_MAX - 1);
    entry->tag[LOG_TAG_MAX - 1] = '\0';

    int msg_len = (int)strlen(msg_start);
    int copy_len = msg_len < LOG_MSG_MAX ? msg_len : LOG_MSG_MAX - 1;
    if (copy_len > 0) {
        memcpy(entry->msg, msg_start, copy_len);
        entry->msg[copy_len] = '\0';
    } else {
        entry->msg[0] = '\0';
    }

    return ret;
}

/* ─────────────────────────────────────────────────────────────────── */
/* Public API                                                             */
/* ─────────────────────────────────────────────────────────────────── */

esp_err_t log_buffer_init(void)
{
    /* Check PSRAM availability */
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t buf_size;

    if (psram_free > 64 * 1024) {
        /* Use 80% of free PSRAM */
        buf_size = psram_free * 80 / 100;
        /* Align to sizeof(log_entry_t) */
        buf_size = (buf_size / sizeof(log_entry_t)) * sizeof(log_entry_t);

        s_entries = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (s_entries) {
            s_capacity = (int)(buf_size / sizeof(log_entry_t));
            ESP_LOGI(TAG, "PSRAM log buffer: %d entries, %zu bytes (%.1f%% of %zu KB free)",
                     s_capacity, buf_size,
                     (double)buf_size / (double)psram_free * 100.0,
                     psram_free / 1024);
        } else {
            ESP_LOGW(TAG, "Failed to allocate PSRAM log buffer, falling back to DRAM");
        }
    } else {
        ESP_LOGW(TAG, "No PSRAM detected (free=%zu bytes), using DRAM fallback", psram_free);
    }

    if (s_entries == NULL) {
        /* Fallback: small DRAM buffer */
        buf_size  = 64 * sizeof(log_entry_t);
        s_entries = malloc(buf_size);
        if (!s_entries) return ESP_ERR_NO_MEM;
        s_capacity = 64;
        ESP_LOGI(TAG, "DRAM log buffer: %d entries, %zu bytes", s_capacity, buf_size);
    }

    s_head    = 0;
    s_wrapped = false;

    /* Install vprintf hook (keep serial output via s_orig_vprintf) */
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);
    if (!s_orig_vprintf) {
        ESP_LOGW(TAG, "esp_log_set_vprintf returned NULL, serial log may be lost");
    }

    return ESP_OK;
}

void log_buffer_info(int *capacity, int *count, int *head, bool *wrapped)
{
    portENTER_CRITICAL(&s_lock);
    if (capacity) *capacity = s_capacity;
    if (head)     *head     = s_head;
    if (wrapped)  *wrapped  = s_wrapped;
    if (count) {
        *count = s_wrapped ? s_capacity : s_head;
    }
    portEXIT_CRITICAL(&s_lock);
}

int log_buffer_read(int start, int want, log_entry_t *out)
{
    if (!s_entries || !out || want <= 0) return 0;

    int capacity, count, head;
    bool wrapped;
    log_buffer_info(&capacity, &count, &head, &wrapped);

    if (start >= count) return 0;
    if (start + want > count) want = count - start;

    int oldest = wrapped ? head : 0;
    int base   = (oldest + start) % capacity;

    for (int i = 0; i < want; i++) {
        int src = (base + i) % capacity;
        memcpy(&out[i], &s_entries[src], sizeof(log_entry_t));
    }

    return want;
}

void log_buffer_clear(void)
{
    portENTER_CRITICAL(&s_lock);
    s_head    = 0;
    s_wrapped = false;
    /* Don't zero the buffer — just reset pointers */
    portEXIT_CRITICAL(&s_lock);
}
