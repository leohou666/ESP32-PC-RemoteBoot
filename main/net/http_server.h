/**
 * @file http_server.h
 * @brief REST API server + app-embedded static file server.
 *
 * Endpoints:
 *   GET  /             → index.html (embedded)
 *   GET  /api/status   → system state JSON
 *   POST /api/boot     → {"os":"windows"|"fedora"|"default"}
 *   POST /api/shutdown → soft shutdown
 *   POST /api/force_off→ force power off (4s relay)
 *   POST /api/reboot   → restart PC
 *   POST /api/reboot_os→ {"os":"windows"|"fedora"} reboot + select OS
 *   GET  /api/update/info → OTA runtime info
 *   POST /api/update   → raw firmware upload (.bin)
 *   GET  /api/config   → current config (password masked)
 *   PUT  /api/config   → update config JSON
 *   GET  /api/netstat  → campus net status
 *   POST /api/auth_now → trigger immediate auth
 */
#pragma once

#include "ihal.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    IRelayController *relay;   /*!< Injected relay controller   */
    void             *boot_mgr; /*!< Injected boot_manager handle */
} http_server_deps_t;

/**
 * @brief Start the HTTP server on port 80.
 *        Serves embedded web assets and OTA endpoints.
 */
esp_err_t http_server_start(const http_server_deps_t *deps);

/** @brief Stop the HTTP server. */
void http_server_stop(void);

#ifdef __cplusplus
}
#endif
