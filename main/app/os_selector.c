/**
 * @file os_selector.c
 * @brief GRUB OS selection via USB HID keyboard sequences.
 */
#include "os_selector.h"

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG "os_sel"

typedef struct {
    os_selector_config_t cfg;
} os_sel_t;

/* ─────────────────────────────────────────────────────────────────── */

esp_err_t os_selector_create(const os_selector_config_t *cfg, void **out)
{
    os_sel_t *s = calloc(1, sizeof(os_sel_t));
    if (!s) return ESP_ERR_NO_MEM;
    s->cfg = *cfg;
    *out = s;
    ESP_LOGI(TAG, "OS selector created: WIN=%d FED=%d DEFAULT=%d wait=%lums",
             cfg->grub_win_idx, cfg->grub_fed_idx,
             cfg->grub_default_idx, cfg->grub_wait_ms);
    return ESP_OK;
}

esp_err_t os_selector_select(void *handle, os_target_t target)
{
    os_sel_t *s = (os_sel_t *)handle;
    if (!s) return ESP_ERR_INVALID_ARG;

    if (target == OS_TARGET_DEFAULT) {
        ESP_LOGI(TAG, "OS_TARGET_DEFAULT: not sending keys, GRUB will timeout");
        return ESP_OK;
    }

    uint8_t target_idx = (target == OS_TARGET_WINDOWS)
                         ? s->cfg.grub_win_idx
                         : s->cfg.grub_fed_idx;

    /* Wait for GRUB to render */
    vTaskDelay(pdMS_TO_TICKS(s->cfg.grub_wait_ms));

    int delta = (int)target_idx - (int)s->cfg.grub_default_idx;
    uint8_t arrow_key = (delta >= 0) ? RB_HID_KEY_DOWN_ARROW : RB_HID_KEY_UP_ARROW;
    int steps = (delta >= 0) ? delta : -delta;

    ESP_LOGI(TAG, "GRUB: current=%d target=%d delta=%d (%s x%d)",
             s->cfg.grub_default_idx, target_idx, delta,
             delta >= 0 ? "DOWN" : "UP", steps);

    for (int i = 0; i < steps; i++) {
        esp_err_t err = s->cfg.keyboard->send_key(s->cfg.keyboard, arrow_key, 80);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Key send failed at step %d: %s", i, esp_err_to_name(err));
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(120));  /* GRUB needs time to process */
    }

    /* Press Enter to confirm selection */
    esp_err_t err = s->cfg.keyboard->send_key(s->cfg.keyboard, RB_HID_KEY_RETURN, 80);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "GRUB selection sent: %s",
                 target == OS_TARGET_WINDOWS ? "Windows" : "Fedora");
    }
    return err;
}

void os_selector_destroy(void *handle)
{
    if (handle) free(handle);
}
