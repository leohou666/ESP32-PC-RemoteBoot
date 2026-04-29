/**
 * @file relay_controller.c
 * @brief GPIO relay controller implementation.
 *
 * Relay modules are commonly active-LOW (GPIO LOW → relay energised).
 * Polarity is configurable at creation time.
 *
 * IMPORTANT: To prevent relay from auto-triggering during ESP32 power-loss,
 * internal pull resistors are enabled:
 *   - Active-LOW  → pull-up  (keeps pin HIGH when ESP is off)
 *   - Active-HIGH → pull-down (keeps pin LOW when ESP is off)
 *
 * For complete protection, add external 10kΩ resistors matching the
 * pull direction to each GPIO pin.
 */
#include "relay_controller.h"

#include <stdlib.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG "relay"

/* ─────────────────────────────────────────────────────────────────── */

typedef struct {
    IRelayController iface;    /* MUST be first — allows safe cast */
    uint8_t gpio[2];           /* gpio[RELAY_POWER], gpio[RELAY_RESET] */
    bool    active_low;        /* true = active-LOW, false = active-HIGH */
} relay_ctrl_t;

/* ─────────────────────────────────────────────────────────────────── */

static uint32_t relay_active_level(relay_ctrl_t *r)
{
    return r->active_low ? 0 : 1;
}

static uint32_t relay_idle_level(relay_ctrl_t *r)
{
    return r->active_low ? 1 : 0;
}

static void relay_press(IRelayController *self, uint8_t relay_id, uint32_t duration_ms)
{
    relay_ctrl_t *r = (relay_ctrl_t *)self;
    if (relay_id > 1) {
        ESP_LOGE(TAG, "Invalid relay id %d", relay_id);
        return;
    }
    uint8_t pin = r->gpio[relay_id];
    ESP_LOGI(TAG, "Relay %d (GPIO %d) press for %lu ms (active=%s)",
             relay_id, pin, duration_ms, r->active_low ? "LOW" : "HIGH");

    gpio_set_level(pin, relay_active_level(r));
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    gpio_set_level(pin, relay_idle_level(r));
}

/* ─────────────────────────────────────────────────────────────────── */

esp_err_t relay_controller_create(uint8_t gpio_relay1,
                                  uint8_t gpio_relay2,
                                  bool active_low,
                                  IRelayController **out)
{
    relay_ctrl_t *r = calloc(1, sizeof(relay_ctrl_t));
    if (!r) return ESP_ERR_NO_MEM;

    r->gpio[RELAY_POWER] = gpio_relay1;
    r->gpio[RELAY_RESET] = gpio_relay2;
    r->active_low        = active_low;

    /* Configure both GPIO pins as outputs.
     * Enable pull resistors opposite to the active level so that
     * during ESP32 reset / power-loss the relay stays OFF. */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_relay1) | (1ULL << gpio_relay2),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) { free(r); return err; }

    /* Ensure relays are idle (not pressed) at startup */
    gpio_set_level(gpio_relay1, relay_idle_level(r));
    gpio_set_level(gpio_relay2, relay_idle_level(r));

    r->iface.ctx   = r;
    r->iface.press = relay_press;

    *out = &r->iface;
    ESP_LOGI(TAG, "Relay controller ready: PWR=GPIO%d RST=GPIO%d active=%s %s",
             gpio_relay1, gpio_relay2,
             active_low ? "LOW" : "HIGH",
             active_low ? "(pull-up)" : "(pull-down)");
    return ESP_OK;
}

void relay_controller_destroy(IRelayController *ctrl)
{
    if (ctrl) free(ctrl);
}
