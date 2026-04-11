/**
 * @file ihal.h
 * @brief HAL interface definitions (Dependency Inversion Principle)
 *
 * All application-layer code depends on these interfaces,
 * not on concrete hardware implementations.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ─────────────────────────────────────────────────────────────
 * Relay Controller Interface
 * ───────────────────────────────────────────────────────────── */

/** Relay IDs */
#define RELAY_POWER  0   /*!< PWR_SW — JFP1 power button */
#define RELAY_RESET  1   /*!< RST_SW — JFP1 reset button */

typedef struct IRelayController IRelayController;
struct IRelayController {
    void *ctx;
    /**
     * @brief Press a relay for the given duration.
     * @param self  Instance pointer
     * @param relay_id  RELAY_POWER or RELAY_RESET
     * @param duration_ms  Press duration in milliseconds
     */
    void (*press)(IRelayController *self, uint8_t relay_id, uint32_t duration_ms);
};

/* ─────────────────────────────────────────────────────────────
 * POST Detector Interface
 * ───────────────────────────────────────────────────────────── */

/** Callback fired when POST is detected as complete (GRUB menu visible). */
typedef void (*post_complete_cb_t)(void *ctx);

typedef struct IPostDetector IPostDetector;
struct IPostDetector {
    void *ctx;
    /** @brief Start monitoring the HDD_LED signal. Calls cb when POST is done. */
    void (*start)(IPostDetector *self, post_complete_cb_t cb, void *cb_ctx);
    /** @brief Stop monitoring (releases GPIO interrupt). */
    void (*stop)(IPostDetector *self);
    /** @brief Returns true if the HDD_LED line is currently active (PC on). */
    bool (*is_pc_powered)(IPostDetector *self);
};

/* ─────────────────────────────────────────────────────────────
 * USB HID Keyboard Interface
 * ───────────────────────────────────────────────────────────── */

/** HID keycodes used by the OS selector */
#define RB_HID_KEY_RETURN      0x28
#define RB_HID_KEY_DELETE      0x4C   /*!< DEL key — BIOS setup on most boards */
#define RB_HID_KEY_F2          0x3B   /*!< F2 key — BIOS setup on some boards  */
#define RB_HID_KEY_UP_ARROW    0x52
#define RB_HID_KEY_DOWN_ARROW  0x51

typedef struct IUsbHidKeyboard IUsbHidKeyboard;
struct IUsbHidKeyboard {
    void *ctx;
    /**
     * @brief Press and release a single key (no modifier).
     * @param keycode  HID keycode (e.g. RB_HID_KEY_DOWN_ARROW)
     * @param press_ms Duration to hold key (typically 50ms)
     */
    esp_err_t (*send_key)(IUsbHidKeyboard *self, uint8_t keycode, uint32_t press_ms);
};
