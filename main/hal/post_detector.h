/**
 * @file post_detector.h
 * @brief POST completion detector via HDD_LED signal on JFP1.
 *
 * During POST, BIOS reads from storage → HDD_LED blinks frequently.
 * When GRUB menu appears, HDD_LED goes quiet.
 * A sustained quiet period (configurable) triggers the POST-complete callback.
 */
#pragma once

#include "ihal.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  gpio_hdd_led;    /*!< GPIO connected to JFP1 HDD_LED+ */
    uint32_t warmup_ms;       /*!< Ignore HDD_LED for this long after start */
    uint32_t quiet_thresh_ms; /*!< Silence duration to declare POST complete */
    uint32_t timeout_ms;      /*!< Max wait; fires error if exceeded */
} post_detector_config_t;

/**
 * @brief Create a POST detector instance.
 * @param cfg    Configuration parameters
 * @param[out] out  Populated interface pointer
 */
esp_err_t post_detector_create(const post_detector_config_t *cfg,
                               IPostDetector **out);

/** @brief Destroy a POST detector instance. */
void post_detector_destroy(IPostDetector *det);

#ifdef __cplusplus
}
#endif
