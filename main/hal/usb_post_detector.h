/**
 * @file usb_post_detector.h
 * @brief POST completion detector via USB bus handshake events.
 *
 * Instead of monitoring HDD_LED pulses on GPIO, this detector watches the
 * USB mount/unmount lifecycle.  Typical PC boot sequence observed on the bus:
 *
 *   Power-on → (nothing for a few seconds)
 *            → BIOS initialises USB → first tud_mount_cb()
 *            → BIOS hands off to GRUB → USB bus reset
 *            → tud_umount_cb() → tud_mount_cb()   (second mount)
 *            → GRUB is now listening for keyboard input
 *
 * Detection algorithm:
 *   1. After start(), wait for the FIRST mount event.
 *   2. Arm a "settle" timer (configurable, default 2 s).
 *      - If an unmount+remount cycle occurs, restart the timer
 *        (the remount is a strong signal that the bootloader took over).
 *   3. When the settle timer expires with USB still mounted,
 *      fire the POST-complete callback — GRUB is ready.
 *
 * This completely eliminates the dependency on GPIO / HDD_LED wiring.
 */
#pragma once

#include "ihal.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t settle_ms;   /*!< Time to wait after last mount before declaring POST done (ms).
                               Allows the bus to stabilise after BIOS→GRUB transition.
                               Recommended: 2000–5000 ms. */
    uint32_t timeout_ms;  /*!< Maximum total wait time from start() to POST-complete (ms).
                               Fires error if exceeded. */
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
