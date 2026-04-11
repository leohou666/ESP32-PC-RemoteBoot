/**
 * @file boot_manager.h
 * @brief PC boot flow state machine.
 *
 * Orchestrates: relay press → POST wait → OS select → online check.
 * All steps are asynchronous; completion is signaled via callbacks or
 * by reading the system_state.
 */
#pragma once

#include "ihal.h"
#include "os_selector.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    IRelayController *relay;       /*!< Hardware relay control         */
    IPostDetector    *post_det;    /*!< POST completion detector       */
    void             *os_sel;      /*!< os_selector handle             */
    IUsbHidKeyboard  *keyboard;    /*!< Direct keyboard access (for BIOS entry) */
    uint32_t          relay_press_ms;    /*!< Normal power press duration */
    uint32_t          relay_force_ms;    /*!< Force-off press duration    */
    uint32_t          relay_reset_ms;    /*!< Reset press duration        */
    const char       *pc_ip;       /*!< PC IP for online detection (can be NULL) */
} boot_manager_config_t;

/**
 * @brief Create the boot manager.
 * @param cfg   Configuration with injected dependencies
 * @param[out] out  Handle
 */
esp_err_t boot_manager_create(const boot_manager_config_t *cfg, void **out);

/**
 * @brief Initiate a boot sequence.
 * @param handle      boot_manager handle
 * @param target_os   Which OS to select at GRUB
 * @return ESP_ERR_INVALID_STATE if PC is already booting or online
 */
esp_err_t boot_manager_boot(void *handle, os_target_t target_os);

/**
 * @brief Send soft shutdown command (short power press).
 * @return ESP_ERR_INVALID_STATE if PC is not ONLINE
 */
esp_err_t boot_manager_shutdown(void *handle);

/**
 * @brief Force power off (hold relay 4s).
 */
esp_err_t boot_manager_force_off(void *handle);

/**
 * @brief Press reset button briefly.
 */
esp_err_t boot_manager_reset(void *handle);

/**
 * @brief Reboot and select a specific OS (reset + POST wait + OS select).
 */
esp_err_t boot_manager_reboot_os(void *handle, os_target_t target_os);

/** @brief Destroy boot manager. */
void boot_manager_destroy(void *handle);

#ifdef __cplusplus
}
#endif
