/**
 * @file system_state.h
 * @brief Thread-safe global system state.
 *
 * Single-writer / multiple-reader model using a mutex.
 * All modules read/write PC and campus-net state through this API.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */

typedef enum {
    PC_STATE_OFFLINE       = 0,  /*!< PC is powered off              */
    PC_STATE_POWERING_ON   = 1,  /*!< Relay pressed, waiting POST    */
    PC_STATE_POST_RUNNING  = 2,  /*!< POST in progress               */
    PC_STATE_SELECTING_OS  = 3,  /*!< Sending GRUB keyboard sequence */
    PC_STATE_BOOTING       = 4,  /*!< OS loading, not yet pingable   */
    PC_STATE_ONLINE        = 5,  /*!< OS up, PC reachable on network */
    PC_STATE_SHUTTING_DOWN = 6,  /*!< Shutdown command sent          */
    PC_STATE_ERROR         = 7,  /*!< Unexpected error               */
} pc_state_t;

typedef enum {
    CAMPUS_NET_UNKNOWN    = 0,
    CAMPUS_NET_OK         = 1,
    CAMPUS_NET_AUTH_NEEDED = 2,
    CAMPUS_NET_ERROR      = 3,
} campus_net_state_t;

typedef enum {
    BOOTED_OS_UNKNOWN  = 0,
    BOOTED_OS_WINDOWS  = 1,
    BOOTED_OS_FEDORA   = 2,
} booted_os_t;

typedef struct {
    pc_state_t          pc_state;
    campus_net_state_t  campus_net;
    booted_os_t         os_booted;
    time_t              last_auth_ts;   /*!< Unix timestamp of last successful auth */
    time_t              boot_start_ts;  /*!< When the current boot sequence started */
    char                error_msg[80];  /*!< Human-readable error (if ERROR state)  */
} system_state_t;

/* ------------------------------------------------------------------ */

/** @brief Initialise the state module (creates internal mutex). */
esp_err_t system_state_init(void);

/** @brief Atomically read a snapshot of current state. */
void system_state_get(system_state_t *out);

/** @brief Set the PC state. */
void system_state_set_pc(pc_state_t state);

/** @brief Set campus-net state + update last_auth_ts if OK. */
void system_state_set_campus_net(campus_net_state_t state);

/** @brief Set which OS was booted. */
void system_state_set_os(booted_os_t os);

/** @brief Set error message and transition to ERROR state. */
void system_state_set_error(const char *msg);

/** @brief Convenience: return current PC state. */
pc_state_t system_state_pc(void);

/** @brief Return human-readable PC state string (for API/debug). */
const char *system_state_pc_str(pc_state_t s);

/** @brief Return human-readable campus net state string. */
const char *system_state_campus_str(campus_net_state_t s);

#ifdef __cplusplus
}
#endif
