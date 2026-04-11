/**
 * @file usb_hid_keyboard.h
 * @brief USB HID keyboard implementation using ESP-IDF TinyUSB.
 *
 * ESP32-S3 native USB (GPIO19/20) acts as a USB keyboard.
 * Used to navigate the GRUB boot menu after POST completion.
 *
 * Hardware: Connect ESP32-S3 USB-C (native OTG) to PC USB-A port.
 */
#pragma once

#include "ihal.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise TinyUSB HID keyboard and install USB driver.
 *        Must be called once at startup. Blocks up to 2s for USB enumeration.
 *
 * @param[out] out  Populated interface pointer
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if PC hasn't enumerated yet
 */
esp_err_t usb_hid_keyboard_create(IUsbHidKeyboard **out);

/** @brief Destroy HID keyboard instance and uninstall USB driver. */
void usb_hid_keyboard_destroy(IUsbHidKeyboard *kbd);

/* ─────────────────────────────────────────────────────────────────── */
/* USB mount event observation (for POST detection via USB handshake)   */
/* ─────────────────────────────────────────────────────────────────── */

/** USB bus events observable by external modules. */
typedef enum {
    USB_EVENT_MOUNTED,     /*!< Host has enumerated and mounted this device */
    USB_EVENT_UNMOUNTED,   /*!< Host has dropped / reset the bus            */
    USB_EVENT_SUSPENDED,   /*!< Host suspended the device                  */
    USB_EVENT_RESUMED,     /*!< Host resumed the device                    */
} usb_mount_event_t;

/** Callback type for mount events. */
typedef void (*usb_mount_event_cb_t)(usb_mount_event_t event, void *arg);

/**
 * @brief Register a callback to receive USB mount/unmount notifications.
 *        Up to 4 callbacks can be registered.  Must be called before boot.
 */
void usb_hid_register_mount_cb(usb_mount_event_cb_t cb, void *arg);

#ifdef __cplusplus
}
#endif
