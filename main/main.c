/**
 * @file main.c
 * @brief RemoteBoot firmware entry point.
 *
 * Dependency Injection assembly:
 *   HAL layer  → Network layer → Application layer
 *
 * Boot sequence:
 *   1. NVS init + config load
 *   2. HAL init (relay, post_detector, usb_hid)
 *   3. WiFi connect
 *   4. Campus net keepalive start
 *   5. HTTP server start (REST API + Web UI)
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "inttypes.h"
#include "esp_netif.h"
#include "esp_event.h"

/* Project modules */
#include "config_manager.h"
#include "system_state.h"
#include "relay_controller.h"
#include "post_detector.h"
#include "usb_post_detector.h"
#include "usb_hid_keyboard.h"
#include "tinyusb.h"
#include "wifi_manager.h"
#include "campus_net_auth.h"
#include "http_server.h"
#include "os_selector.h"
#include "boot_manager.h"
#include "ota_manager.h"
#include "log_buffer.h"

/* Kconfig-defined defaults */
#include "sdkconfig.h"

#define TAG "main"

/* ─────────────────────────────────────────────────────────────────── */
/* Kconfig → NVS defaults for runtime-configurable keys                 */
/* ─────────────────────────────────────────────────────────────────── */
#ifdef CONFIG_RB_RELAY_ACTIVE_LOW
  #define RB_DEF_RELAY_POLARITY 0
#else
  #define RB_DEF_RELAY_POLARITY 1
#endif

#ifdef CONFIG_RB_BOOTLOADER_WINDOWS
  #define RB_DEF_BOOTLOADER_TYPE 1
#else
  #define RB_DEF_BOOTLOADER_TYPE 0
#endif

/* ─────────────────────────────────────────────────────────────────── */
/* USB ↔ PC state auto-sync (polling-based)                              */
/*                                                                        */
/* Polls TinyUSB device state every 3 s.  Uses BOTH tud_mounted() AND     */
/* tud_suspended() because many desktop PCs keep USB VBUS powered during  */
/* soft-off — so unplug never fires and cfg_num never clears.  When the   */
/* PC shuts down, the host stops sending SOF; the device enters suspend   */
/* within 3 ms.  A sustained suspend (3 consecutive polls = 9 s) is       */
/* treated as "PC off".                                                   */
/*                                                                        */
/* Only acts on OFFLINE ↔ ONLINE edges — never interferes with active     */
/* boot sequences (POWERING_ON / POST_RUNNING / SELECTING_OS).            */
/* ─────────────────────────────────────────────────────────────────── */
#define USB_SYNC_INTERVAL_S   3
#define SUSPEND_DEBOUNCE_MAX  3   /* 3 × 3 s = 9 s sustained suspend → off */

static int s_suspend_debounce = 0;

static void usb_sync_timer_cb(void *arg)
{
    bool active  = tud_mounted() && !tud_suspended();  /* PC alive, bus active */
    bool susp    = tud_suspended();                     /* bus idle (host gone) */
    pc_state_t st = system_state_pc();

    if (active && st == PC_STATE_OFFLINE) {
        s_suspend_debounce = 0;
        ESP_LOGI(TAG, "USB active but state is OFFLINE — PC must be online");
        system_state_set_pc(PC_STATE_ONLINE);
        system_state_set_os(BOOTED_OS_UNKNOWN);
        return;
    }

    if (!active && st == PC_STATE_ONLINE) {
        /* Immediate: unmount / cable unplug */
        s_suspend_debounce = 0;
        ESP_LOGI(TAG, "USB unmounted while ONLINE — PC must be offline");
        system_state_set_pc(PC_STATE_OFFLINE);
        system_state_set_os(BOOTED_OS_UNKNOWN);
        return;
    }

    /* Suspended while ONLINE: host may have gone away (PC soft-off) */
    if (susp && st == PC_STATE_ONLINE) {
        s_suspend_debounce++;
        if (s_suspend_debounce >= SUSPEND_DEBOUNCE_MAX) {
            ESP_LOGI(TAG, "USB suspended for %ds — PC must be offline",
                     SUSPEND_DEBOUNCE_MAX * USB_SYNC_INTERVAL_S);
            system_state_set_pc(PC_STATE_OFFLINE);
            system_state_set_os(BOOTED_OS_UNKNOWN);
            s_suspend_debounce = 0;
        }
        return;
    }

    /* Not suspended (bus active) — reset debounce */
    s_suspend_debounce = 0;
}

/* ─────────────────────────────────────────────────────────────────── */
/* WiFi callbacks                                                        */
/* ─────────────────────────────────────────────────────────────────── */
static void on_wifi_connected(void *ctx)
{
    char ip[16];
    wifi_manager_get_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "WiFi connected! IP: %s  — Web UI: http://%s", ip, ip);
    /* Trigger an immediate campus net check */
    campus_net_auth_now();
}

static void on_wifi_disconnected(void *ctx)
{
    ESP_LOGW(TAG, "WiFi disconnected");
}

/* ─────────────────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "RemoteBoot firmware starting...");

    /* ── 1. Config & state init ─────────────────────────────────── */
    ESP_ERROR_CHECK(config_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(system_state_init());
    ESP_ERROR_CHECK(config_ensure_api_token());
    ESP_ERROR_CHECK(ota_manager_init());
    ESP_ERROR_CHECK(log_buffer_init());

    /* ── Read runtime config (NVS with Kconfig defaults) ───────── */
    uint8_t relay_pol  = config_get_u8(CFG_KEY_RELAY_POLARITY, RB_DEF_RELAY_POLARITY);
    uint8_t btldr_type = config_get_u8(CFG_KEY_BOOTLOADER_TYPE, RB_DEF_BOOTLOADER_TYPE);
    bool is_windows_bt = (btldr_type != 0);  /* 0=GRUB, 1=Windows */
    bool relay_active_low = (relay_pol == 0);

    ESP_LOGI(TAG, "Config: relay=%s btldr=%s",
             relay_active_low ? "active-LOW" : "active-HIGH",
             is_windows_bt ? "Windows" : "GRUB");

    /* ── 2. HAL: Relay controller ──────────────────────────────── */
    uint8_t gpio_r1 = config_get_u8(CFG_KEY_GPIO_RELAY1, CONFIG_RB_GPIO_RELAY1);
    uint8_t gpio_r2 = config_get_u8(CFG_KEY_GPIO_RELAY2, CONFIG_RB_GPIO_RELAY2);

    IRelayController *relay = NULL;
    ESP_ERROR_CHECK(relay_controller_create(gpio_r1, gpio_r2, relay_active_low, &relay));

    /* ── 4. HAL: USB HID keyboard ──────────────────────────── */
    IUsbHidKeyboard *keyboard = NULL;
    esp_err_t usb_err = usb_hid_keyboard_create(&keyboard);
    if (usb_err != ESP_OK) {
        ESP_LOGW(TAG, "USB HID keyboard init failed (%s). GRUB selection disabled.",
                 esp_err_to_name(usb_err));
    }

    /* Start USB ↔ PC state auto-sync (polls tud_mounted() every 3 s) */
    esp_timer_create_args_t sync_timer_args = {
        .callback = usb_sync_timer_cb,
        .arg      = NULL,
        .name     = "usb_sync",
    };
    esp_timer_handle_t sync_timer = NULL;
    esp_timer_create(&sync_timer_args, &sync_timer);
    esp_timer_start_periodic(sync_timer, (uint64_t)USB_SYNC_INTERVAL_S * 1000000);

    /* ── 3. HAL: POST detector (mode selected in Kconfig) ──────── */
    IPostDetector *post_det = NULL;
#if CONFIG_RB_POST_DETECT_USB
    uint16_t post_settle = config_get_u16(CFG_KEY_POST_SETTLE_MS, CONFIG_RB_USB_POST_SETTLE_MS);

    ESP_LOGI(TAG, "POST detection mode: USB bus handshake (mode=%s settle=%ums)",
             is_windows_bt ? "simple" : "GRUB", post_settle);
    usb_post_detector_config_t upost_cfg = {
        .settle_ms           = post_settle,
        .timeout_ms          = CONFIG_RB_POST_TIMEOUT_MS,
        .first_mount_is_post = is_windows_bt,
    };
    ESP_ERROR_CHECK(usb_post_detector_create(&upost_cfg, &post_det));
#else
    ESP_LOGI(TAG, "POST detection mode: HDD LED GPIO");
    uint8_t gpio_post = config_get_u8(CFG_KEY_GPIO_POST, CONFIG_RB_GPIO_POST_DETECT);
    post_detector_config_t post_cfg = {
        .gpio_hdd_led    = gpio_post,
        .warmup_ms       = config_get_u16("post_warmup", CONFIG_RB_POST_WARMUP_MS),
        .quiet_thresh_ms = config_get_u16(CFG_KEY_HDD_QUIET_MS,
                                          CONFIG_RB_POST_QUIET_MS),
        .timeout_ms      = CONFIG_RB_POST_TIMEOUT_MS,
    };
    ESP_ERROR_CHECK(post_detector_create(&post_cfg, &post_det));
#endif

    /* ── 5. App: OS selector (GRUB only) ──────────────────────── */
    void *os_sel = NULL;
    if (!is_windows_bt && keyboard) {
        os_selector_config_t os_cfg = {
            .keyboard         = keyboard,
            .grub_win_idx     = config_get_u8(CFG_KEY_GRUB_WIN_IDX,
                                              CONFIG_RB_GRUB_WIN_INDEX),
            .grub_fed_idx     = config_get_u8(CFG_KEY_GRUB_FED_IDX,
                                              CONFIG_RB_GRUB_FED_INDEX),
            .grub_default_idx = config_get_u8(CFG_KEY_GRUB_DEFAULT,
                                              CONFIG_RB_GRUB_DEFAULT_INDEX),
            .grub_wait_ms     = config_get_u16(CFG_KEY_GRUB_WAIT_MS,
                                               CONFIG_RB_GRUB_WAIT_MS),
        };
        ESP_ERROR_CHECK(os_selector_create(&os_cfg, &os_sel));
        ESP_LOGI(TAG, "GRUB mode: OS selector created");
    } else if (is_windows_bt) {
        ESP_LOGI(TAG, "Windows Boot Manager mode: OS selector skipped");
    }

    /* ── 6. App: Boot manager ────────────────────────────────── */
    boot_manager_config_t bm_cfg = {
        .relay          = relay,
        .post_det       = post_det,
        .os_sel         = os_sel,
        .keyboard       = keyboard,
        .relay_press_ms = CONFIG_RB_RELAY_SOFT_PRESS_MS,
        .relay_force_ms = CONFIG_RB_RELAY_HARD_PRESS_MS,
        .relay_reset_ms = CONFIG_RB_RELAY_RESET_PRESS_MS,
    };
    void *boot_mgr = NULL;
    ESP_ERROR_CHECK(boot_manager_create(&bm_cfg, &boot_mgr));

    /* ── 7. Network: WiFi ────────────────────────────────────── */
    wifi_manager_callbacks_t wifi_cbs = {
        .on_connected    = on_wifi_connected,
        .on_disconnected = on_wifi_disconnected,
        .cb_ctx          = NULL,
    };
    esp_err_t wifi_err = wifi_manager_start(&wifi_cbs);
    if (wifi_err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "No WiFi configured. Set SSID/password via /api/config "
                      "after connecting to the access point.");
        /* TODO P2: Start SoftAP captive portal for first-time config */
    }

    /* ── 8. Network: Campus keepalive ────────────────────────── */
    campus_net_config_t campus_cfg = {
        .portal_host      = CONFIG_RB_CAMPUS_PORTAL_HOST,
        .portal_port      = CONFIG_RB_CAMPUS_PORTAL_PORT,
        .check_interval_s = config_get_u16(CFG_KEY_PING_INTERVAL_S,
                                           CONFIG_RB_CAMPUS_PING_INTERVAL_S),
        .retry_interval_s = CONFIG_RB_CAMPUS_AUTH_RETRY_S,
    };
    ESP_ERROR_CHECK(campus_net_start(&campus_cfg));

    /* ── 9. Network: HTTP server (REST API + Web UI) ─────────── */
    http_server_deps_t http_deps = {
        .relay    = relay,
        .boot_mgr = boot_mgr,
    };
    ESP_ERROR_CHECK(http_server_start(&http_deps));

    esp_err_t ota_err = ota_manager_mark_running_app_valid();
    if (ota_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to confirm running OTA image: %s", esp_err_to_name(ota_err));
    }

    /* ── Done ────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "RemoteBoot ready. Connect to WiFi and navigate to:");
    ESP_LOGI(TAG, "  http://<ESP32_IP>  (local network)");
    ESP_LOGI(TAG, "All tasks running. app_main() returns.");
}
