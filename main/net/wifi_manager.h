/**
 * @file wifi_manager.h
 * @brief WiFi STA connection manager with automatic reconnection.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Callback type for WiFi connection events */
typedef void (*wifi_connected_cb_t)(void *ctx);
typedef void (*wifi_disconnected_cb_t)(void *ctx);

typedef struct {
    wifi_connected_cb_t    on_connected;
    wifi_disconnected_cb_t on_disconnected;
    void                  *cb_ctx;
} wifi_manager_callbacks_t;

/**
 * @brief Initialise and start the WiFi manager.
 *        Reads SSID/password from NVS (config_manager).
 *        Registers event handlers for auto-reconnect.
 */
esp_err_t wifi_manager_start(const wifi_manager_callbacks_t *cbs);

/** @brief Return true if currently connected to WiFi with a valid IP. */
bool wifi_manager_is_connected(void);

/** @brief Trigger a reconnect attempt immediately. */
void wifi_manager_reconnect(void);

/** @brief Get current IP address as string (empty if not connected). */
void wifi_manager_get_ip(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
