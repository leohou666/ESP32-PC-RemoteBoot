/**
 * @file os_selector.h
 * @brief GRUB OS selection via USB HID keyboard.
 *
 * Given a target OS, computes the relative arrow-key delta from the
 * GRUB default highlighted entry and sends the appropriate keystrokes.
 */
#pragma once

#include "ihal.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OS_TARGET_DEFAULT = -1,   /*!< Don't intervene; let GRUB timeout */
    OS_TARGET_WINDOWS =  0,
    OS_TARGET_FEDORA  =  1,
    OS_TARGET_BIOS    =  2,   /*!< Enter BIOS setup (spam DEL/F2 during POST) */
} os_target_t;

typedef struct {
    IUsbHidKeyboard *keyboard;
    uint8_t  grub_win_idx;      /*!< GRUB menu index for Windows (0-based) */
    uint8_t  grub_fed_idx;      /*!< GRUB menu index for Fedora  (0-based) */
    uint8_t  grub_default_idx;  /*!< GRUB currently-highlighted entry      */
    uint32_t grub_wait_ms;      /*!< Delay before sending keys (ms)        */
} os_selector_config_t;

/**
 * @brief Create an OS selector instance.
 * @param cfg   Configuration (reads GRUB indices from config)
 * @param[out] out  Handle
 */
esp_err_t os_selector_create(const os_selector_config_t *cfg, void **out);

/**
 * @brief Select the target OS by sending keyboard events.
 *        Blocks until key sequence is sent (or error).
 *
 * @param handle  os_selector handle
 * @param target  Which OS to select
 */
esp_err_t os_selector_select(void *handle, os_target_t target);

/** @brief Destroy an os_selector instance. */
void os_selector_destroy(void *handle);

#ifdef __cplusplus
}
#endif
