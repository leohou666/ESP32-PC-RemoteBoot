/**
 * @file usb_post_detector.h
 * @brief POST completion detector via USB bus handshake events.
 *
 * Two detection strategies:
 *
 *   GRUB mode (first_mount_is_post = false):
 *     Power-on → BIOS mounts USB → unmount (BIOS→GRUB handoff) →
 *     remount (GRUB) → settle timer → POST done.
 *
 *   Windows Boot Manager mode (first_mount_is_post = true):
 *     Power-on → BIOS mounts USB → settle timer → POST done.
 *     First mount is enough because the Windows bootloader doesn't
 *     reset the USB bus like GRUB does.
 *
 * This completely eliminates the dependency on GPIO / HDD_LED wiring.
 */
#pragma once

#include "ihal.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t settle_ms;          /*!< Time to wait after last mount before declaring POST done (ms). */
    uint32_t timeout_ms;         /*!< Maximum total wait time from start() to POST-complete (ms). */
    bool     first_mount_is_post;/*!< If true, first USB mount = POST done after settle.
                                      If false (GRUB), wait for mount→unmount→remount cycle. */
} usb_post_detector_config_t;

/**
 * @brief Create a USB-handshake-based POST detector instance.
 * @param cfg    Configuration parameters
 * @param[out] out  Populated IPostDetector interface pointer
 */
esp_err_t usb_post_detector_create(const usb_post_detector_config_t *cfg,
                                   IPostDetector **out);

/** @brief Destroy USB POST detector instance. */
void usb_post_detector_destroy(IPostDetector *det);

#ifdef __cplusplus
}
#endif
