/**
 * @file config_manager.c
 * @brief NVS-backed configuration manager implementation.
 */
#include "config_manager.h"

#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"

#define TAG       "config"
#define NVS_NS    "rb"

static nvs_handle_t s_nvs = 0;

/* ─────────────────────────────────────────────────────────────────── */

esp_err_t config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition full or version changed, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", NVS_NS, esp_err_to_name(err));
    }
    return err;
}

/* ─────────────────────────────────────────────────────────────────── */

esp_err_t config_get_str(const char *key, char *buf, size_t len)
{
    return nvs_get_str(s_nvs, key, buf, &len);
}

esp_err_t config_set_str(const char *key, const char *val)
{
    esp_err_t err = nvs_set_str(s_nvs, key, val);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    return err;
}

uint16_t config_get_u16(const char *key, uint16_t def_val)
{
    uint16_t v = def_val;
    nvs_get_u16(s_nvs, key, &v);   /* silently returns def on ERR_NOT_FOUND */
    return v;
}

esp_err_t config_set_u16(const char *key, uint16_t val)
{
    esp_err_t err = nvs_set_u16(s_nvs, key, val);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    return err;
}

uint8_t config_get_u8(const char *key, uint8_t def_val)
{
    uint8_t v = def_val;
    nvs_get_u8(s_nvs, key, &v);
    return v;
}

esp_err_t config_set_u8(const char *key, uint8_t val)
{
    esp_err_t err = nvs_set_u8(s_nvs, key, val);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    return err;
}

/* ─────────────────────────────────────────────────────────────────── */

esp_err_t config_ensure_api_token(void)
{
    char token[33];
    esp_err_t err = config_get_str(CFG_KEY_API_TOKEN, token, sizeof(token));
    
    if (err != ESP_OK) {
        /* Generate 16 random bytes → 32 hex chars */
        uint32_t rnd[4];
        for (int i = 0; i < 4; i++) rnd[i] = esp_random();
        snprintf(token, sizeof(token),
                 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32,
                 rnd[0], rnd[1], rnd[2], rnd[3]);

        err = config_set_str(CFG_KEY_API_TOKEN, token);
        if (err != ESP_OK) return err;
    }

    /* Print prominently so user can copy from serial monitor */
    ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  API Token (copy to Web UI):         ║");
    ESP_LOGI(TAG, "║  %-36s ║", token);
    ESP_LOGI(TAG, "╚══════════════════════════════════════╝");
    
    return ESP_OK;
}
