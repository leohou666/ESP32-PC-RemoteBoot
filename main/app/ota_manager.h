/**
 * @file ota_manager.h
 * @brief OTA update helper built on top of ESP-IDF app_update APIs.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_ota_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_MANAGER_PART_LABEL_LEN 17
#define OTA_MANAGER_STATE_LEN      24

typedef struct {
    char project_name[32];
    char version[32];
    char idf_ver[32];
    char build_date[16];
    char build_time[16];
    char running_partition[OTA_MANAGER_PART_LABEL_LEN];
    char boot_partition[OTA_MANAGER_PART_LABEL_LEN];
    char next_partition[OTA_MANAGER_PART_LABEL_LEN];
    char ota_state[OTA_MANAGER_STATE_LEN];
    bool rollback_pending;
    bool update_supported;
} ota_runtime_info_t;

typedef struct {
    esp_ota_handle_t      handle;
    const esp_partition_t *partition;
    size_t                bytes_written;
    bool                  active;
} ota_session_t;

esp_err_t ota_manager_init(void);
esp_err_t ota_manager_begin(ota_session_t *session, size_t image_size);
esp_err_t ota_manager_write(ota_session_t *session, const void *data, size_t size);
esp_err_t ota_manager_finish(ota_session_t *session);
esp_err_t ota_manager_abort(ota_session_t *session);
esp_err_t ota_manager_get_runtime_info(ota_runtime_info_t *out);
esp_err_t ota_manager_mark_running_app_valid(void);

#ifdef __cplusplus
}
#endif
