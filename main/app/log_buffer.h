/**
 * @file log_buffer.h
 * @brief Ring buffer for runtime log capture in PSRAM.
 *
 * Hooks into esp_log_set_vprintf() to capture all ESP_LOGx output
 * into a circular buffer allocated from PSRAM. Automatically sizes
 * the buffer to ~80% of available PSRAM at init time.
 *
 * Entries are fixed-size (128 bytes) for simplicity:
 *   timestamp_ms (8B) + level (1B) + tag (7B) + message (112B)
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_ENTRY_SIZE   128
#define LOG_MSG_MAX      112
#define LOG_TAG_MAX      8    /* stored as 7 chars + 1 null pad */

typedef struct {
    int64_t  timestamp_ms;          /*!< esp_timer_get_time() / 1000     */
    uint8_t  level;                 /*!< 'E','W','I','D','V'            */
    char     tag[LOG_TAG_MAX];      /*!< Log tag, truncated to 7 chars   */
    char     msg[LOG_MSG_MAX];      /*!< Log message, truncated          */
} log_entry_t;

/**
 * @brief Initialise the log buffer.
 *
 * Allocates ~80% of free PSRAM for the ring buffer.
 * Installs the vprintf hook to capture all ESP_LOGx output.
 * Falls back to internal DRAM if PSRAM is unavailable.
 *
 * @return ESP_OK on success
 */
esp_err_t log_buffer_init(void);

/**
 * @brief Get ring buffer metadata.
 * @param[out] capacity   Total number of slots
 * @param[out] count      Number of entries currently stored
 * @param[out] head       Index of the most recent entry (0 = oldest if not wrapped)
 * @param[out] wrapped    Whether the buffer has wrapped at least once
 */
void log_buffer_info(int *capacity, int *count, int *head, bool *wrapped);

/**
 * @brief Read a range of entries from the ring buffer.
 *
 * Entries are ordered oldest-to-newest. If @p start + @p want exceeds
 * the stored count, fewer entries are returned.
 *
 * @param start    Start index (0 = oldest entry)
 * @param want     Maximum number of entries to read
 * @param[out] out Buffer to copy entries into
 * @return Actual number of entries copied
 */
int log_buffer_read(int start, int want, log_entry_t *out);

/**
 * @brief Clear all entries in the ring buffer.
 */
void log_buffer_clear(void);

#ifdef __cplusplus
}
#endif
