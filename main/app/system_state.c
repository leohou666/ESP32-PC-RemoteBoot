/**
 * @file system_state.c
 * @brief Thread-safe global system state implementation.
 */
#include "system_state.h"

#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#define TAG "state"

static system_state_t  s_state  = {0};
static SemaphoreHandle_t s_mutex = NULL;

/* ─────────────────────────────────────────────────────────────────── */

esp_err_t system_state_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    memset(&s_state, 0, sizeof(s_state));
    return ESP_OK;
}

void system_state_get(system_state_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(out, &s_state, sizeof(s_state));
    xSemaphoreGive(s_mutex);
}

void system_state_set_pc(pc_state_t state)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.pc_state = state;
    if (state == PC_STATE_POWERING_ON) {
        s_state.boot_start_ts = time(NULL);
    }
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "PC state → %s", system_state_pc_str(state));
}

void system_state_set_campus_net(campus_net_state_t state)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.campus_net = state;
    if (state == CAMPUS_NET_OK) {
        s_state.last_auth_ts = time(NULL);
    }
    xSemaphoreGive(s_mutex);
}

void system_state_set_os(booted_os_t os)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.os_booted = os;
    xSemaphoreGive(s_mutex);
}

void system_state_set_error(const char *msg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.pc_state = PC_STATE_ERROR;
    snprintf(s_state.error_msg, sizeof(s_state.error_msg), "%s", msg);
    xSemaphoreGive(s_mutex);
    ESP_LOGE(TAG, "Error: %s", msg);
}

pc_state_t system_state_pc(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    pc_state_t s = s_state.pc_state;
    xSemaphoreGive(s_mutex);
    return s;
}

const char *system_state_pc_str(pc_state_t s)
{
    switch (s) {
        case PC_STATE_OFFLINE:       return "offline";
        case PC_STATE_POWERING_ON:   return "powering_on";
        case PC_STATE_POST_RUNNING:  return "post_running";
        case PC_STATE_SELECTING_OS:  return "selecting_os";
        case PC_STATE_BOOTING:       return "booting";
        case PC_STATE_ONLINE:        return "online";
        case PC_STATE_SHUTTING_DOWN: return "shutting_down";
        case PC_STATE_ERROR:         return "error";
        default:                     return "unknown";
    }
}

const char *system_state_campus_str(campus_net_state_t s)
{
    switch (s) {
        case CAMPUS_NET_UNKNOWN:     return "unknown";
        case CAMPUS_NET_OK:          return "ok";
        case CAMPUS_NET_AUTH_NEEDED: return "auth_needed";
        case CAMPUS_NET_ERROR:       return "error";
        default:                     return "unknown";
    }
}
