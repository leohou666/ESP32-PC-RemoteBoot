/**
 * @file relay_controller.h
 * @brief Concrete implementation of IRelayController using ESP32 GPIO.
 */
#pragma once

#include "ihal.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create and initialise a relay controller instance.
 *
 * Configures GPIO pins as push-pull outputs (active HIGH to energise relay).
 *
 * @param gpio_relay1  GPIO for Relay 1 (PWR_SW)
 * @param gpio_relay2  GPIO for Relay 2 (RST_SW)
 * @param[out] out     Populated interface pointer
 * @return ESP_OK on success
 */
esp_err_t relay_controller_create(uint8_t gpio_relay1,
                                  uint8_t gpio_relay2,
                                  IRelayController **out);

/** @brief Destroy a relay controller instance and free resources. */
void relay_controller_destroy(IRelayController *ctrl);

#ifdef __cplusplus
}
#endif
