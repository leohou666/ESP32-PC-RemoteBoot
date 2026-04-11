/**
 * @file config_manager.h
 * @brief NVS-backed configuration manager.
 *
 * Provides typed get/set operations over the "rb" NVS namespace.
 * All modules read runtime configuration through this API.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** NVS key definitions — centralised here to avoid magic strings */
#define CFG_KEY_WIFI_SSID        "wifi_ssid"
#define CFG_KEY_WIFI_PASS        "wifi_pass"
#define CFG_KEY_CAMPUS_CARD      "campus_card"
#define CFG_KEY_CAMPUS_PASS      "campus_pass"
#define CFG_KEY_API_TOKEN        "api_token"
#define CFG_KEY_PC_IP            "pc_ip"
#define CFG_KEY_GRUB_WIN_IDX     "grub_win_idx"
#define CFG_KEY_GRUB_FED_IDX     "grub_fed_idx"
#define CFG_KEY_GRUB_DEFAULT     "grub_default"
#define CFG_KEY_GRUB_WAIT_MS     "grub_wait_ms"
#define CFG_KEY_PING_INTERVAL_S  "ping_intv_s"
#define CFG_KEY_HDD_QUIET_MS     "hdd_quiet_ms"
#define CFG_KEY_GPIO_RELAY1      "gpio_relay1"
#define CFG_KEY_GPIO_RELAY2      "gpio_relay2"
#define CFG_KEY_GPIO_POST        "gpio_post"

/**
 * @brief Initialise NVS flash and open the "rb" namespace.
 *        Must be called once before any other config_* function.
 */
esp_err_t config_init(void);

/** @brief Read a string value. Returns ESP_ERR_NVS_NOT_FOUND if unset. */
esp_err_t config_get_str(const char *key, char *buf, size_t len);

/** @brief Write a string value. */
esp_err_t config_set_str(const char *key, const char *val);

/** @brief Read a uint16 value. Returns @p def_val if key not found. */
uint16_t  config_get_u16(const char *key, uint16_t def_val);

/** @brief Write a uint16 value. */
esp_err_t config_set_u16(const char *key, uint16_t val);

/** @brief Read a uint8 value. Returns @p def_val if key not found. */
uint8_t   config_get_u8(const char *key, uint8_t def_val);

/** @brief Write a uint8 value. */
esp_err_t config_set_u8(const char *key, uint8_t val);

/**
 * @brief Generate and store a random API token if one doesn't exist.
 *        Token is a 32-char hex string. Prints to console on first boot.
 */
esp_err_t config_ensure_api_token(void);

#ifdef __cplusplus
}
#endif
