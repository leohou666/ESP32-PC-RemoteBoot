/**
 * @file post_detector.c
 * @brief POST completion detector via HDD_LED silence window.
 *
 * Approach:
 *  - GPIO interrupt on both edges counts HDD_LED transitions.
 *  - A periodic timer (every 250ms) samples the pulse count.
 *  - After warmup, if samples are consistently zero for quiet_thresh_ms,
 *    the POST-complete callback is fired.
 */
#include "post_detector.h"

#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "post_det"

#define SAMPLE_INTERVAL_MS  250   /* How often to sample pulse count */

typedef enum {
    DET_IDLE,
    DET_WARMUP,
    DET_RUNNING,    /* counting pulses */
    DET_QUIET,      /* monitoring silence */
    DET_DONE,
    DET_TIMEOUT,
} det_state_t;

typedef struct {
    IPostDetector     iface;          /* MUST be first */
    post_detector_config_t cfg;
    post_complete_cb_t cb;
    void              *cb_ctx;

    volatile uint32_t  pulse_count;   /* incremented in ISR */
    det_state_t        state;
    int64_t            quiet_start_us; /* when the last silence window started */
    int64_t            watchdog_start_us;

    esp_timer_handle_t sample_timer;
    TaskHandle_t       task;
} post_det_t;

/* ─────────────────────────────────────────────────────────────────── */
/* ISR: just count transitions                                          */
/* ─────────────────────────────────────────────────────────────────── */
static void IRAM_ATTR hdd_led_isr(void *arg)
{
    post_det_t *d = (post_det_t *)arg;
    d->pulse_count++;
}

/* ─────────────────────────────────────────────────────────────────── */
/* Timer callback: 4x per second, advances the state machine            */
/* ─────────────────────────────────────────────────────────────────── */
static void sample_timer_cb(void *arg)
{
    post_det_t *d = (post_det_t *)arg;
    int64_t now_us = esp_timer_get_time();

    /* Drain pulse counter atomically */
    uint32_t pulses = d->pulse_count;
    d->pulse_count  = 0;

    /* Timeout watchdog */
    if ((now_us - d->watchdog_start_us) > (int64_t)d->cfg.timeout_ms * 1000) {
        if (d->state != DET_DONE && d->state != DET_TIMEOUT) {
            d->state = DET_TIMEOUT;
            ESP_LOGW(TAG, "POST detection timed out");
        }
        return;
    }

    switch (d->state) {
        case DET_WARMUP:
            /* Stay in warmup until time elapsed */
            if ((now_us - d->watchdog_start_us) >= (int64_t)d->cfg.warmup_ms * 1000) {
                ESP_LOGI(TAG, "Warmup done, monitoring HDD_LED...");
                d->state = DET_RUNNING;
                d->quiet_start_us = 0;
            }
            break;

        case DET_RUNNING:
            if (pulses > 0) {
                /* Disk activity detected → POST still running */
                d->quiet_start_us = 0;
            } else {
                /* First sample with no pulses */
                if (d->quiet_start_us == 0) {
                    d->quiet_start_us = now_us;
                    d->state = DET_QUIET;
                }
            }
            break;

        case DET_QUIET:
            if (pulses > 0) {
                /* Disk activity resumed → reset silence window */
                d->quiet_start_us = 0;
                d->state = DET_RUNNING;
            } else {
                int64_t quiet_dur = now_us - d->quiet_start_us;
                if (quiet_dur >= (int64_t)d->cfg.quiet_thresh_ms * 1000) {
                    ESP_LOGI(TAG, "POST complete! HDD_LED silent for %lld ms",
                             quiet_dur / 1000);
                    d->state = DET_DONE;
                    /* Fire callback from a short task to avoid ISR context */
                    if (d->cb) d->cb(d->cb_ctx);
                }
            }
            break;

        default:
            break;
    }
}

/* ─────────────────────────────────────────────────────────────────── */

static void det_start(IPostDetector *self, post_complete_cb_t cb, void *cb_ctx)
{
    post_det_t *d = (post_det_t *)self;
    d->cb       = cb;
    d->cb_ctx   = cb_ctx;
    d->pulse_count = 0;
    d->state    = DET_WARMUP;
    d->watchdog_start_us = esp_timer_get_time();
    d->quiet_start_us    = 0;

    /* Install GPIO interrupt */
    gpio_set_intr_type(d->cfg.gpio_hdd_led, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(d->cfg.gpio_hdd_led, hdd_led_isr, d);

    /* Start sampling timer */
    esp_timer_start_periodic(d->sample_timer,
                             (uint64_t)SAMPLE_INTERVAL_MS * 1000);
    ESP_LOGI(TAG, "POST detector started (GPIO %d)", d->cfg.gpio_hdd_led);
}

static void det_stop(IPostDetector *self)
{
    post_det_t *d = (post_det_t *)self;
    esp_timer_stop(d->sample_timer);
    gpio_isr_handler_remove(d->cfg.gpio_hdd_led);
    d->state = DET_IDLE;
    ESP_LOGI(TAG, "POST detector stopped");
}

static bool det_is_pc_powered(IPostDetector *self)
{
    post_det_t *d = (post_det_t *)self;
    /* If we're past warmup, we assume PC is powered */
    return (d->state != DET_IDLE);
}

/* ─────────────────────────────────────────────────────────────────── */

esp_err_t post_detector_create(const post_detector_config_t *cfg,
                               IPostDetector **out)
{
    post_det_t *d = calloc(1, sizeof(post_det_t));
    if (!d) return ESP_ERR_NO_MEM;
    memcpy(&d->cfg, cfg, sizeof(*cfg));

    /* Configure GPIO as input with pull-down */
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << cfg->gpio_hdd_led,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,   /* enabled in start() */
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) { free(d); return err; }

    /* Install ISR service (safe to call multiple times) */
    gpio_install_isr_service(0);

    /* Create periodic sample timer */
    esp_timer_create_args_t timer_args = {
        .callback = sample_timer_cb,
        .arg      = d,
        .name     = "post_sample",
    };
    err = esp_timer_create(&timer_args, &d->sample_timer);
    if (err != ESP_OK) { free(d); return err; }

    d->iface.ctx          = d;
    d->iface.start        = det_start;
    d->iface.stop         = det_stop;
    d->iface.is_pc_powered = det_is_pc_powered;

    *out = &d->iface;
    ESP_LOGI(TAG, "POST detector created (GPIO %d, quiet=%lu ms)",
             cfg->gpio_hdd_led, cfg->quiet_thresh_ms);
    return ESP_OK;
}

void post_detector_destroy(IPostDetector *det)
{
    if (!det) return;
    post_det_t *d = (post_det_t *)det;
    det_stop(det);
    esp_timer_delete(d->sample_timer);
    free(d);
}
