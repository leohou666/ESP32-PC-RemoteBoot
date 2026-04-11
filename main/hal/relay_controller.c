/**
 * @file relay_controller.c
 * @brief GPIO relay controller implementation.
 *
 * Relay modules are typically active-HIGH (GPIO HIGH → relay energised → NC
 * contacts close, simulating a button press). Adjust RELAY_ACTIVE_LEVEL if
 * your relay module uses active-LOW logic.
 */
#include "relay_controller.h"

#include <stdlib.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG               "relay"
#define RELAY_ACTIVE_LEVEL  1    /* 1 = active HIGH, 0 = active LOW */
#define RELAY_IDLE_LEVEL   (!RELAY_ACTIVE_LEVEL)

/* ─────────────────────────────────────────────────────────────────── */

typedef struct {
    IRelayController iface;    /* MUST be first — allows safe cast */
    uint8_t gpio[2];           /* gpio[RELAY_POWER], gpio[RELAY_RESET] */
} relay_ctrl_t;

/* ─────────────────────────────────────────────────────────────────── */

static void relay_press(IRelayController *self, uint8_t relay_id, uint32_t duration_ms)
{
    relay_ctrl_t *r = (relay_ctrl_t *)self;
    if (relay_id > 1) {
        ESP_LOGE(TAG, "Invalid relay id %d", relay_id);
        return;
    }
    uint8_t pin = r->gpio[relay_id];
    ESP_LOGI(TAG, "Relay %d (GPIO %d) press for %lu ms", relay_id, pin, duration_ms);

    gpio_set_level(pin, RELAY_ACTIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    gpio_set_level(pin, RELAY_IDLE_LEVEL);
}

/* ─────────────────────────────────────────────────────────────────── */

esp_err_t relay_controller_create(uint8_t gpio_relay1,
                                  uint8_t gpio_relay2,
                                  IRelayController **out)
{
    relay_ctrl_t *r = calloc(1, sizeof(relay_ctrl_t));
    if (!r) return ESP_ERR_NO_MEM;

    r->gpio[RELAY_POWER] = gpio_relay1;
    r->gpio[RELAY_RESET] = gpio_relay2;

    /* Configure both GPIO pins as outputs */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_relay1) | (1ULL << gpio_relay2),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) { free(r); return err; }

    /* Ensure relays are idle (not pressed) at startup */
    gpio_set_level(gpio_relay1, RELAY_IDLE_LEVEL);
    gpio_set_level(gpio_relay2, RELAY_IDLE_LEVEL);

    r->iface.ctx   = r;
    r->iface.press = relay_press;

    *out = &r->iface;
    ESP_LOGI(TAG, "Relay controller ready: PWR=GPIO%d RST=GPIO%d", gpio_relay1, gpio_relay2);
    return ESP_OK;
}

void relay_controller_destroy(IRelayController *ctrl)
{
    if (ctrl) free(ctrl);
}
