/**
 * @file boot_manager.c
 * @brief PC boot flow state machine implementation.
 *
 * Sequence:
 *  boot()       → relay press (PWR_SW) → wait POST → os_select → BOOTING
 *  shutdown()   → relay short press (PWR_SW)
 *  force_off()  → relay long press 4s (PWR_SW)
 *  reset()      → relay press (RST_SW)
 *  reboot_os()  → relay short press → wait POST → os_select
 */
#include "boot_manager.h"
#include "system_state.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define TAG "boot_mgr"

typedef struct {
    boot_manager_config_t cfg;
    os_target_t           pending_os;
    TaskHandle_t          boot_task;
} boot_mgr_t;

#include "ping/ping_sock.h"
#include "lwip/inet.h"

/* ─────────────────────────────────────────────────────────────────── */
/* Ping callbacks for PC detection                                      */
/* ─────────────────────────────────────────────────────────────────── */
static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    boot_mgr_t *m = (boot_mgr_t *)args;
    /* Notify the monitoring task that ping succeeded! */
    if (m->boot_task != NULL) {
        xTaskNotifyGive(m->boot_task);
    }
}
static void on_ping_timeout(esp_ping_handle_t hdl, void *args) { }
static void on_ping_end(esp_ping_handle_t hdl, void *args) { }

/* ─────────────────────────────────────────────────────────────────── */
/* BIOS entry task — spams DEL/F2 immediately after power-on            */
/* ─────────────────────────────────────────────────────────────────── */
static void bios_entry_task(void *arg)
{
    boot_mgr_t *m = (boot_mgr_t *)arg;

    if (!m->cfg.keyboard) {
        ESP_LOGE(TAG, "No keyboard available for BIOS entry!");
        system_state_set_error("No USB keyboard for BIOS entry");
        m->boot_task = NULL;
        vTaskDelete(NULL);
        return;
    }

#if CONFIG_RB_BIOS_KEY_F2
    const uint8_t bios_key = RB_HID_KEY_F2;
    const char *key_name = "F2";
#else
    const uint8_t bios_key = RB_HID_KEY_DELETE;
    const char *key_name = "DEL";
#endif

    const uint32_t duration_ms  = CONFIG_RB_BIOS_SPAM_DURATION_MS;
    const uint32_t interval_ms  = CONFIG_RB_BIOS_KEY_INTERVAL_MS;
    const int total_presses = (int)(duration_ms / interval_ms);

    ESP_LOGI(TAG, "BIOS entry: spamming %s key for %lu ms (%d presses)",
             key_name, duration_ms, total_presses);

    system_state_set_pc(PC_STATE_POST_RUNNING);

    for (int i = 0; i < total_presses; i++) {
        esp_err_t err = m->cfg.keyboard->send_key(m->cfg.keyboard, bios_key, 50);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "BIOS key %s press %d/%d", key_name, i + 1, total_presses);
        } else {
            ESP_LOGW(TAG, "BIOS key send failed at press %d: %s",
                     i + 1, esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }

    /* After spamming, we assume BIOS is entered.
     * Set state to ONLINE so the user can interact via force_off/reset */
    system_state_set_pc(PC_STATE_ONLINE);
    system_state_set_os(BOOTED_OS_UNKNOWN);
    ESP_LOGI(TAG, "BIOS entry complete. State -> ONLINE");

    m->boot_task = NULL;
    vTaskDelete(NULL);
}

static void boot_monitoring_task(void *arg)
{
    boot_mgr_t *m = (boot_mgr_t *)arg;

    if (m->cfg.pc_ip != NULL && strlen(m->cfg.pc_ip) > 0) {
        ESP_LOGI(TAG, "Starting ICMP ping monitor to %s...", m->cfg.pc_ip);
        
        esp_ping_callbacks_t cbs = {
            .cb_args = m,
            .on_ping_success = on_ping_success,
            .on_ping_timeout = on_ping_timeout,
            .on_ping_end = on_ping_end
        };
        esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
        config.count = ESP_PING_COUNT_INFINITE; // Ping infinitely until success
        config.interval_ms = 1000;
        ipaddr_aton(m->cfg.pc_ip, &config.target_addr);
        
        esp_ping_handle_t ping_hdl;
        esp_ping_new_session(&config, &cbs, &ping_hdl);
        esp_ping_start(ping_hdl);

        // Wait up to 5 minutes for PC to boot and respond
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(300000)) > 0) {
            pc_state_t cur = system_state_pc();
            if (cur == PC_STATE_BOOTING || cur == PC_STATE_SELECTING_OS) {
                system_state_set_pc(PC_STATE_ONLINE);
                ESP_LOGI(TAG, "PC ping successful! State -> ONLINE");
            }
        } else {
            ESP_LOGW(TAG, "PC ping timed out after 5 minutes.");
        }
        
        esp_ping_stop(ping_hdl);
        esp_ping_delete_session(ping_hdl);
    } else {
        /* Fallback if no PC IP is configured */
        ESP_LOGW(TAG, "No PC IP configured. Using 15s generic timeout...");
        vTaskDelay(pdMS_TO_TICKS(15000));
        
        if (system_state_pc() == PC_STATE_BOOTING) {
            system_state_set_pc(PC_STATE_ONLINE);
            ESP_LOGI(TAG, "OS presumed booted (timeout). State -> ONLINE");
        }
    }
    
    m->boot_task = NULL;
    vTaskDelete(NULL);
}

/* ─────────────────────────────────────────────────────────────────── */
/* POST-complete callback — called from post_detector timer context     */
/* ─────────────────────────────────────────────────────────────────── */
static void on_post_complete(void *ctx)
{
    boot_mgr_t *m = (boot_mgr_t *)ctx;
    system_state_set_pc(PC_STATE_SELECTING_OS);

    esp_err_t err = os_selector_select(m->cfg.os_sel, m->pending_os);
    if (err != ESP_OK) {
        system_state_set_error("OS key selection failed");
        return;
    }

    m->cfg.post_det->stop(m->cfg.post_det);
    system_state_set_pc(PC_STATE_BOOTING);

    /* Update global OS state for Web UI */
    booted_os_t booted_os = BOOTED_OS_UNKNOWN;
    if (m->pending_os == OS_TARGET_WINDOWS)      booted_os = BOOTED_OS_WINDOWS;
    else if (m->pending_os == OS_TARGET_FEDORA)  booted_os = BOOTED_OS_FEDORA;
    system_state_set_os(booted_os);

    ESP_LOGI(TAG, "OS selected, PC is booting");

    /* Spawn monitoring task to eventually transition to ONLINE */
    if (m->boot_task == NULL) {
        xTaskCreate(boot_monitoring_task, "boot_mon", 2048, m, 5, &m->boot_task);
    }
}

/* ─────────────────────────────────────────────────────────────────── */

esp_err_t boot_manager_create(const boot_manager_config_t *cfg, void **out)
{
    boot_mgr_t *m = calloc(1, sizeof(boot_mgr_t));
    if (!m) return ESP_ERR_NO_MEM;
    m->cfg         = *cfg;
    if (cfg->pc_ip) {
        m->cfg.pc_ip = strdup(cfg->pc_ip);
    }
    m->pending_os  = OS_TARGET_DEFAULT;
    m->boot_task   = NULL;
    *out           = m;
    ESP_LOGI(TAG, "Boot manager ready");
    return ESP_OK;
}

esp_err_t boot_manager_boot(void *handle, os_target_t target_os)
{
    boot_mgr_t *m = (boot_mgr_t *)handle;
    pc_state_t cur = system_state_pc();

    if (cur != PC_STATE_OFFLINE && cur != PC_STATE_ERROR) {
        ESP_LOGW(TAG, "Boot rejected: PC state is %s", system_state_pc_str(cur));
        return ESP_ERR_INVALID_STATE;
    }

    m->pending_os = target_os;
    system_state_set_pc(PC_STATE_POWERING_ON);

    /* Press power button */
    m->cfg.relay->press(m->cfg.relay, RELAY_POWER, m->cfg.relay_press_ms);

    if (target_os == OS_TARGET_BIOS) {
        /* BIOS entry: skip POST detection, immediately spam DEL/F2 */
        ESP_LOGI(TAG, "Power pressed. Entering BIOS setup...");
        if (m->boot_task == NULL) {
            xTaskCreate(bios_entry_task, "bios_entry", 3072, m, 5, &m->boot_task);
        }
    } else {
        /* Normal boot: start POST detector, wait for GRUB */
        m->cfg.post_det->start(m->cfg.post_det, on_post_complete, m);
        system_state_set_pc(PC_STATE_POST_RUNNING);
        ESP_LOGI(TAG, "Power pressed. Waiting for POST complete...");
    }
    return ESP_OK;
}

esp_err_t boot_manager_shutdown(void *handle)
{
    boot_mgr_t *m = (boot_mgr_t *)handle;
    pc_state_t cur = system_state_pc();

    if (cur != PC_STATE_ONLINE) {
        ESP_LOGW(TAG, "Shutdown rejected: PC not online (state=%s)",
                 system_state_pc_str(cur));
        return ESP_ERR_INVALID_STATE;
    }

    system_state_set_pc(PC_STATE_SHUTTING_DOWN);
    m->cfg.relay->press(m->cfg.relay, RELAY_POWER, m->cfg.relay_press_ms);
    /* PC will transition to OFFLINE once we stop seeing it on the network */
    /* For now, we optimistically set OFFLINE; a monitoring task would confirm */
    vTaskDelay(pdMS_TO_TICKS(1000));
    system_state_set_pc(PC_STATE_OFFLINE);
    return ESP_OK;
}

esp_err_t boot_manager_force_off(void *handle)
{
    boot_mgr_t *m = (boot_mgr_t *)handle;
    /* Stop any ongoing POST detection */
    m->cfg.post_det->stop(m->cfg.post_det);
    system_state_set_pc(PC_STATE_SHUTTING_DOWN);
    m->cfg.relay->press(m->cfg.relay, RELAY_POWER, m->cfg.relay_force_ms);
    vTaskDelay(pdMS_TO_TICKS(500));
    system_state_set_pc(PC_STATE_OFFLINE);
    ESP_LOGW(TAG, "Force off complete");
    return ESP_OK;
}

esp_err_t boot_manager_reset(void *handle)
{
    boot_mgr_t *m = (boot_mgr_t *)handle;
    m->cfg.relay->press(m->cfg.relay, RELAY_RESET, m->cfg.relay_reset_ms);
    ESP_LOGI(TAG, "Reset button pressed");
    return ESP_OK;
}

esp_err_t boot_manager_reboot_os(void *handle, os_target_t target_os)
{
    boot_mgr_t *m = (boot_mgr_t *)handle;
    m->pending_os = target_os;

    /* Trigger reset and wait for POST */
    m->cfg.post_det->start(m->cfg.post_det, on_post_complete, m);
    m->cfg.relay->press(m->cfg.relay, RELAY_RESET, m->cfg.relay_reset_ms);
    system_state_set_pc(PC_STATE_POST_RUNNING);
    ESP_LOGI(TAG, "Reboot + OS select initiated");
    return ESP_OK;
}

void boot_manager_destroy(void *handle)
{
    boot_mgr_t *m = (boot_mgr_t *)handle;
    if (m) {
        if (m->cfg.pc_ip) free((void *)m->cfg.pc_ip);
        free(m);
    }
}
