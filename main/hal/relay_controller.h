/**
 * @file relay_controller.h
 * @brief Concrete implementation of IRelayController using ESP32 GPIO.
 */
#pragma once

#include "ihal.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create and initialise a relay controller instance.
 *
 * @param gpio_relay1  GPIO for Relay 1 (PWR_SW)
 * @param gpio_relay2  GPIO for Relay 2 (RST_SW)
 * @param active_low   true if relay module is active-LOW (most common).
 *                     false if active-HIGH. Determines pull direction
 *                     to prevent spurious trigger during power-loss.
 * @param[out] out     Populated interface pointer
 * @return ESP_OK on success
 */
esp_err_t relay_controller_create(uint8_t gpio_relay1,
                                  uint8_t gpio_relay2,
                                  bool active_low,
                                  IRelayController **out);

/** @brief Destroy a relay controller instance and free resources. */
void relay_controller_destroy(IRelayController *ctrl);

#ifdef __cplusplus
}
#endif
