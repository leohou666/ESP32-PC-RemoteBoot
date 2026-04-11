/**
 * @file wifi_manager.c
 * @brief WiFi STA manager with automatic exponential-backoff reconnection.
 */
#include "wifi_manager.h"
#include "config_manager.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define TAG "wifi"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_RETRY           10

static EventGroupHandle_t    s_wifi_eg;
static wifi_manager_callbacks_t s_cbs = {0};
static bool                  s_connected = false;
static char                  s_ip_str[16] = {0};
static int                   s_retry = 0;

/* ─────────────────────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        memset(s_ip_str, 0, sizeof(s_ip_str));
        if (s_cbs.on_disconnected) s_cbs.on_disconnected(s_cbs.cb_ctx);

        if (s_retry < MAX_RETRY) {
            uint32_t delay_ms = 2000 * (1 << s_retry);   /* exponential back-off */
            if (delay_ms > 60000) delay_ms = 60000;
            ESP_LOGW(TAG, "WiFi disconnected. Retry %d/%d in %lu ms",
                     s_retry + 1, MAX_RETRY, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            esp_wifi_connect();
            s_retry++;
        } else {
            ESP_LOGE(TAG, "Max retries exceeded. Set FAIL bit.");
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
            /* Reset retry for next manual reconnect */
            s_retry = 0;
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_str);
        s_retry     = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        if (s_cbs.on_connected) s_cbs.on_connected(s_cbs.cb_ctx);
    }
}

/* ─────────────────────────────────────────────────────────────────── */

esp_err_t wifi_manager_start(const wifi_manager_callbacks_t *cbs)
{
    if (cbs) s_cbs = *cbs;

    char ssid[64] = {0};
    char pass[64] = {0};
    config_get_str(CFG_KEY_WIFI_SSID, ssid, sizeof(ssid));
    config_get_str(CFG_KEY_WIFI_PASS, pass, sizeof(pass));

    /* Fallback to menuconfig defaults if NVS is empty */
    if (ssid[0] == '\0') {
#ifdef CONFIG_RB_WIFI_SSID
        if (strlen(CONFIG_RB_WIFI_SSID) > 0) {
            strncpy(ssid, CONFIG_RB_WIFI_SSID, sizeof(ssid) - 1);
            strncpy(pass, CONFIG_RB_WIFI_PASS, sizeof(pass) - 1);
            ESP_LOGI(TAG, "Using default WiFi credentials from Kconfig");
        }
#endif
    }

    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "No WiFi SSID configured! Please configure via menuconfig 'Default Network Config' and recompile.");
        return ESP_ERR_NOT_FOUND;
    }

    s_wifi_eg = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid,     ssid, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    return ESP_OK;
}

bool wifi_manager_is_connected(void) { return s_connected; }

void wifi_manager_reconnect(void)
{
    s_retry = 0;
    esp_wifi_connect();
}

void wifi_manager_get_ip(char *buf, size_t len)
{
    strncpy(buf, s_ip_str, len - 1);
    buf[len - 1] = '\0';
}
