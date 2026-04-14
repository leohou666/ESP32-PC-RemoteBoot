/**
 * @file ota_manager.c
 * @brief OTA update helper built on top of ESP-IDF app_update APIs.
 */
#include "ota_manager.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"

#define TAG "ota_mgr"

static SemaphoreHandle_t s_ota_mutex = NULL;

static void session_reset(ota_session_t *session)
{
    if (!session) return;
    memset(session, 0, sizeof(*session));
}

static void copy_partition_label(char *dst, size_t len, const esp_partition_t *part)
{
    if (len == 0) return;
    if (!part) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, len, "%s", part->label);
}

static const char *ota_state_str(esp_ota_img_states_t state)
{
    switch (state) {
        case ESP_OTA_IMG_NEW:            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_VALID:          return "valid";
        case ESP_OTA_IMG_INVALID:        return "invalid";
        case ESP_OTA_IMG_ABORTED:        return "aborted";
        case ESP_OTA_IMG_UNDEFINED:      return "undefined";
        default:                         return "unknown";
    }
}

static void ota_unlock(void)
{
    if (s_ota_mutex) {
        xSemaphoreGive(s_ota_mutex);
    }
}

esp_err_t ota_manager_init(void)
{
    if (s_ota_mutex) return ESP_OK;
    s_ota_mutex = xSemaphoreCreateMutex();
    return s_ota_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ota_manager_begin(ota_session_t *session, size_t image_size)
{
    if (!session) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(ota_manager_init(), TAG, "init failed");

    if (xSemaphoreTake(s_ota_mutex, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    session_reset(session);

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        ota_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    size_t erase_size = image_size > 0 ? image_size : OTA_WITH_SEQUENTIAL_WRITES;
    esp_err_t err = esp_ota_begin(next, erase_size, &session->handle);
    if (err != ESP_OK) {
        ota_unlock();
        return err;
    }

    session->partition = next;
    session->active = true;
    ESP_LOGI(TAG, "OTA session started on %s (%u bytes requested)",
             next->label, (unsigned)image_size);
    return ESP_OK;
}

esp_err_t ota_manager_write(ota_session_t *session, const void *data, size_t size)
{
    if (!session || !session->active || !data) return ESP_ERR_INVALID_ARG;
    esp_err_t err = esp_ota_write(session->handle, data, size);
    if (err == ESP_OK) {
        session->bytes_written += size;
    }
    return err;
}

esp_err_t ota_manager_finish(ota_session_t *session)
{
    if (!session || !session->active || !session->partition) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_ota_end(session->handle);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(session->partition);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA image ready on %s (%u bytes). Boot target updated.",
                 session->partition->label, (unsigned)session->bytes_written);
    }

    session_reset(session);
    ota_unlock();
    return err;
}

esp_err_t ota_manager_abort(ota_session_t *session)
{
    if (!session || !session->active) return ESP_OK;
    esp_err_t err = esp_ota_abort(session->handle);
    session_reset(session);
    ota_unlock();
    return err;
}

esp_err_t ota_manager_get_runtime_info(ota_runtime_info_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    snprintf(out->project_name, sizeof(out->project_name), "%s", desc->project_name);
    snprintf(out->version, sizeof(out->version), "%s", desc->version);
    snprintf(out->idf_ver, sizeof(out->idf_ver), "%s", desc->idf_ver);
    snprintf(out->build_date, sizeof(out->build_date), "%s", desc->date);
    snprintf(out->build_time, sizeof(out->build_time), "%s", desc->time);
    copy_partition_label(out->running_partition, sizeof(out->running_partition), running);
    copy_partition_label(out->boot_partition, sizeof(out->boot_partition), boot);
    copy_partition_label(out->next_partition, sizeof(out->next_partition), next);
    out->update_supported = (next != NULL);

    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = running ? esp_ota_get_state_partition(running, &ota_state)
                            : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) {
        snprintf(out->ota_state, sizeof(out->ota_state), "%s", ota_state_str(ota_state));
        out->rollback_pending = (ota_state == ESP_OTA_IMG_PENDING_VERIFY);
        return ESP_OK;
    }

    snprintf(out->ota_state, sizeof(out->ota_state), "%s",
             (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_NOT_FOUND) ? "undefined" : "unknown");
    out->rollback_pending = false;
    return ESP_OK;
}

esp_err_t ota_manager_mark_running_app_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;

    if (!running) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "Running partition %s has no OTA state to confirm", running->label);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Running image %s OTA state: %s",
             running->label, ota_state_str(ota_state));

    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Confirming running OTA image as valid");
        return esp_ota_mark_app_valid_cancel_rollback();
    }

    return ESP_OK;
}
