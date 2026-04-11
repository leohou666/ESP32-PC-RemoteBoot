/**
 * @file usb_post_detector.c
 * @brief POST completion detector via USB bus mount/unmount events.
 *
 * See usb_post_detector.h for the algorithm description.
 *
 * State machine:
 *   IDLE  → start() → WAITING_FIRST_MOUNT
 *   WAITING_FIRST_MOUNT  → mount event → SETTLING
 *   SETTLING  → unmount → WAITING_REMOUNT  (BIOS→GRUB transition)
 *   WAITING_REMOUNT → mount event → SETTLING  (restart settle timer)
 *   SETTLING  → settle timer expires → DONE  (fire callback)
 *
 * The settle timer is an esp_timer oneshot.  Every time a new mount
 * event arrives the timer is restarted, so the callback only fires
 * once the bus has been quiet (mounted, no further resets) for
 * settle_ms.
 */
#include "usb_post_detector.h"
#include "usb_hid_keyboard.h"

#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "usb_post"

typedef enum {
    UPOST_IDLE,
    UPOST_WAITING_FIRST_MOUNT,
    UPOST_SETTLING,
    UPOST_WAITING_REMOUNT,
    UPOST_DONE,
    UPOST_TIMEOUT,
} upost_state_t;

typedef struct {
    IPostDetector               iface;          /* MUST be first */
    usb_post_detector_config_t  cfg;
    post_complete_cb_t          cb;
    void                       *cb_ctx;

    volatile upost_state_t      state;
    int                         mount_count;    /* total mount events seen */
    int64_t                     start_time_us;

    esp_timer_handle_t          settle_timer;   /* oneshot: fires when bus stabilises */
    esp_timer_handle_t          timeout_timer;  /* oneshot: overall watchdog          */
} usb_post_det_t;

/* ─────────────────────────────────────────────────────────────────── */
/* Settle timer expired — POST is complete, GRUB is ready              */
/* ─────────────────────────────────────────────────────────────────── */
static void settle_timer_cb(void *arg)
{
    usb_post_det_t *d = (usb_post_det_t *)arg;
    if (d->state == UPOST_SETTLING) {
        int64_t elapsed = (esp_timer_get_time() - d->start_time_us) / 1000;
        ESP_LOGI(TAG, "USB bus settled after %lld ms, %d mount(s). POST complete!",
                 elapsed, d->mount_count);
        d->state = UPOST_DONE;
        esp_timer_stop(d->timeout_timer);
        if (d->cb) d->cb(d->cb_ctx);
    }
}

/* ─────────────────────────────────────────────────────────────────── */
/* Timeout watchdog — POST took too long                                */
/* ─────────────────────────────────────────────────────────────────── */
static void timeout_timer_cb(void *arg)
{
    usb_post_det_t *d = (usb_post_det_t *)arg;
    if (d->state != UPOST_DONE && d->state != UPOST_IDLE) {
        ESP_LOGW(TAG, "USB POST detection timed out (%lu ms)", d->cfg.timeout_ms);
        d->state = UPOST_TIMEOUT;
        esp_timer_stop(d->settle_timer);
    }
}

/* ─────────────────────────────────────────────────────────────────── */
/* USB event callback — subscribed to TinyUSB mount notifications      */
/* ─────────────────────────────────────────────────────────────────── */
static void usb_event_handler(usb_mount_event_t event, void *arg)
{
    usb_post_det_t *d = (usb_post_det_t *)arg;

    switch (event) {
    case USB_EVENT_MOUNTED:
        d->mount_count++;
        ESP_LOGI(TAG, "USB MOUNT #%d (state=%d)", d->mount_count, d->state);

        if (d->state == UPOST_WAITING_FIRST_MOUNT ||
            d->state == UPOST_WAITING_REMOUNT) {

            d->state = UPOST_SETTLING;
            /* Start (or restart) the settle timer */
            esp_timer_stop(d->settle_timer);
            esp_timer_start_once(d->settle_timer,
                                 (uint64_t)d->cfg.settle_ms * 1000);
            ESP_LOGI(TAG, "Settle timer started (%lu ms)", d->cfg.settle_ms);
        } else if (d->state == UPOST_SETTLING) {
            /* Another mount while settling — restart the timer
               (this means there was yet another bus reset) */
            esp_timer_stop(d->settle_timer);
            esp_timer_start_once(d->settle_timer,
                                 (uint64_t)d->cfg.settle_ms * 1000);
            ESP_LOGI(TAG, "Settle timer restarted (extra mount)");
        }
        break;

    case USB_EVENT_UNMOUNTED:
        ESP_LOGI(TAG, "USB UNMOUNT (state=%d)", d->state);

        if (d->state == UPOST_SETTLING) {
            /* BIOS→bootloader transition: bus was reset.
               Stop the settle timer, wait for remount. */
            esp_timer_stop(d->settle_timer);
            d->state = UPOST_WAITING_REMOUNT;
            ESP_LOGI(TAG, "Bus reset detected — waiting for remount (BIOS→GRUB)");
        }
        break;

    case USB_EVENT_SUSPENDED:
    case USB_EVENT_RESUMED:
        /* Informational only — don't affect state machine */
        break;
    }
}

/* ─────────────────────────────────────────────────────────────────── */
/* IPostDetector interface implementation                                */
/* ─────────────────────────────────────────────────────────────────── */
static void upost_start(IPostDetector *self, post_complete_cb_t cb, void *cb_ctx)
{
    usb_post_det_t *d = (usb_post_det_t *)self;
    d->cb           = cb;
    d->cb_ctx       = cb_ctx;
    d->mount_count  = 0;
    d->start_time_us = esp_timer_get_time();
    d->state        = UPOST_WAITING_FIRST_MOUNT;

    /* Start the overall timeout watchdog */
    esp_timer_start_once(d->timeout_timer, (uint64_t)d->cfg.timeout_ms * 1000);

    ESP_LOGI(TAG, "USB POST detector started (settle=%lums, timeout=%lums)",
             d->cfg.settle_ms, d->cfg.timeout_ms);
}

static void upost_stop(IPostDetector *self)
{
    usb_post_det_t *d = (usb_post_det_t *)self;
    esp_timer_stop(d->settle_timer);
    esp_timer_stop(d->timeout_timer);
    d->state = UPOST_IDLE;
    ESP_LOGI(TAG, "USB POST detector stopped");
}

static bool upost_is_pc_powered(IPostDetector *self)
{
    usb_post_det_t *d = (usb_post_det_t *)self;
    return (d->state != UPOST_IDLE);
}

/* ─────────────────────────────────────────────────────────────────── */
/* Create / Destroy                                                      */
/* ─────────────────────────────────────────────────────────────────── */
esp_err_t usb_post_detector_create(const usb_post_detector_config_t *cfg,
                                   IPostDetector **out)
{
    usb_post_det_t *d = calloc(1, sizeof(usb_post_det_t));
    if (!d) return ESP_ERR_NO_MEM;
    memcpy(&d->cfg, cfg, sizeof(*cfg));

    /* Create timers */
    esp_timer_create_args_t settle_args = {
        .callback = settle_timer_cb,
        .arg      = d,
        .name     = "upost_settle",
    };
    esp_err_t err = esp_timer_create(&settle_args, &d->settle_timer);
    if (err != ESP_OK) { free(d); return err; }

    esp_timer_create_args_t timeout_args = {
        .callback = timeout_timer_cb,
        .arg      = d,
        .name     = "upost_timeout",
    };
    err = esp_timer_create(&timeout_args, &d->timeout_timer);
    if (err != ESP_OK) {
        esp_timer_delete(d->settle_timer);
        free(d);
        return err;
    }

    /* Subscribe to USB mount events */
    usb_hid_register_mount_cb(usb_event_handler, d);

    d->iface.ctx          = d;
    d->iface.start        = upost_start;
    d->iface.stop         = upost_stop;
    d->iface.is_pc_powered = upost_is_pc_powered;

    *out = &d->iface;
    ESP_LOGI(TAG, "USB POST detector created (settle=%lums, timeout=%lums)",
             cfg->settle_ms, cfg->timeout_ms);
    return ESP_OK;
}

void usb_post_detector_destroy(IPostDetector *det)
{
    if (!det) return;
    usb_post_det_t *d = (usb_post_det_t *)det;
    upost_stop(det);
    esp_timer_delete(d->settle_timer);
    esp_timer_delete(d->timeout_timer);
    free(d);
}
