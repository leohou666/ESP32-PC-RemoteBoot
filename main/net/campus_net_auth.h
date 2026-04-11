/**
 * @file campus_net_auth.h
 * @brief Campus network keepalive: periodic TCP check → ePortal re-auth.
 *
 * Strategy:
 *   1. Every N seconds, attempt TCP connection to 8.8.8.8:53 (3s timeout).
 *   2. If TCP connect succeeds → internet is up, nothing to do.
 *   3. If TCP connect fails  → send ePortal GET request to re-authenticate.
 *   4. If ePortal also fails → mark campus_net ERROR, retry after M seconds.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *portal_host;   /*!< e.g. "172.30.255.42" */
    uint16_t    portal_port;   /*!< e.g. 801             */
    uint32_t    check_interval_s; /*!< seconds between checks */
    uint32_t    retry_interval_s; /*!< retry delay on failure  */
} campus_net_config_t;

/**
 * @brief Start the campus net keepalive background task.
 *        Reads card/password from NVS via config_manager.
 */
esp_err_t campus_net_start(const campus_net_config_t *cfg);

/** @brief Stop the keepalive task. */
void campus_net_stop(void);

/** @brief Trigger an immediate auth check (called on demand from REST API). */
void campus_net_auth_now(void);

/** @brief Check if internet is currently reachable (cached from last check). */
bool campus_net_is_online(void);

#ifdef __cplusplus
}
#endif
