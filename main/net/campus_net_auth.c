/**
 * @file campus_net_auth.c
 * @brief Campus network keepalive: TCP check + ePortal re-auth.
 */
#include "campus_net_auth.h"
#include "config_manager.h"
#include "system_state.h"
#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#define TAG "campus"

#define AUTH_NOW_BIT  BIT0

static campus_net_config_t s_cfg      = {0};
static TaskHandle_t        s_task     = NULL;
static EventGroupHandle_t  s_eg       = NULL;
static volatile bool       s_online   = false;

/* ─────────────────────────────────────────────────────────────────── */
/* Check internet by attempting TCP connect to 223.5.5.5:53              */
/* ─────────────────────────────────────────────────────────────────── */
static bool check_internet(void)
{
    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int gai = getaddrinfo("223.5.5.5", "53", &hints, &res);
    if (gai != 0 || res == NULL) return false;

    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) { freeaddrinfo(res); return false; }

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    bool ok = (connect(sock, res->ai_addr, res->ai_addrlen) == 0);
    close(sock);
    freeaddrinfo(res);
    return ok;
}

/* ─────────────────────────────────────────────────────────────────── */
/* Send ePortal authentication request                                   */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t do_auth(void)
{
    char card[64] = {0}, pass[64] = {0};
    config_get_str(CFG_KEY_CAMPUS_CARD, card, sizeof(card));
    config_get_str(CFG_KEY_CAMPUS_PASS, pass, sizeof(pass));

    if (card[0] == '\0') {
        ESP_LOGW(TAG, "No campus card configured, skipping auth");
        return ESP_ERR_NOT_FOUND;
    }

    /* Build URL — card is URL-encoded as %2C0%2C{card} per the portal format */
    char url[512];
    snprintf(url, sizeof(url),
        "http://%s:%d/eportal/portal/login"
        "?callback=dr1003&login_method=1"
        "&user_account=%%2C0%%2C%s"
        "&user_password=%s"
        "&wlan_user_ip=&wlan_user_ipv6=&wlan_user_mac=000000000000"
        "&wlan_ac_ip=&wlan_ac_name=&jsVersion=4.1.3"
        "&terminal_type=1&lang=zh-cn&v=5159&lang=zh",
        s_cfg.portal_host, s_cfg.portal_port, card, pass);

    esp_http_client_config_t http_cfg = {
        .url            = url,
        .method         = HTTP_METHOD_GET,
        .timeout_ms     = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);

    /* Set headers matching the original curl command */
    esp_http_client_set_header(client, "Connection",       "keep-alive");
    esp_http_client_set_header(client, "User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/91.0.4472.77 Safari/537.36 Edg/91.0.864.37");
    esp_http_client_set_header(client, "Accept",           "*/*");
    esp_http_client_set_header(client, "Referer",
        "http://172.30.255.42/");
    esp_http_client_set_header(client, "Accept-Language",
        "zh-CN,zh;q=0.9,en;q=0.8,en-GB;q=0.7,en-US;q=0.6");

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Auth request sent, HTTP status: %d", status);
    } else {
        ESP_LOGE(TAG, "Auth request failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* ─────────────────────────────────────────────────────────────────── */
/* Keepalive task                                                        */
/* ─────────────────────────────────────────────────────────────────── */
static void campus_net_task(void *arg)
{
    while (true) {
        /* Wait for interval OR auth_now signal */
        xEventGroupWaitBits(s_eg, AUTH_NOW_BIT, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(s_cfg.check_interval_s * 1000));

        if (!wifi_manager_is_connected()) {
            ESP_LOGD(TAG, "WiFi not connected, skip check");
            continue;
        }

        ESP_LOGD(TAG, "Checking internet...");
        bool online = check_internet();
        s_online = online;

        if (online) {
            ESP_LOGI(TAG, "Internet OK");
            system_state_set_campus_net(CAMPUS_NET_OK);
        } else {
            ESP_LOGW(TAG, "Internet unreachable, triggering campus auth...");
            system_state_set_campus_net(CAMPUS_NET_AUTH_NEEDED);
            esp_err_t err = do_auth();
            if (err == ESP_OK) {
                /* Wait 3s and verify */
                vTaskDelay(pdMS_TO_TICKS(3000));
                if (check_internet()) {
                    s_online = true;
                    system_state_set_campus_net(CAMPUS_NET_OK);
                    ESP_LOGI(TAG, "Re-auth successful");
                } else {
                    system_state_set_campus_net(CAMPUS_NET_ERROR);
                    ESP_LOGE(TAG, "Re-auth sent but internet still unreachable");
                    /* Short retry delay */
                    vTaskDelay(pdMS_TO_TICKS(s_cfg.retry_interval_s * 1000));
                }
            } else {
                system_state_set_campus_net(CAMPUS_NET_ERROR);
            }
        }
    }
}

/* ─────────────────────────────────────────────────────────────────── */


esp_err_t campus_net_start(const campus_net_config_t *cfg)
{
    s_cfg = *cfg;
    s_eg  = xEventGroupCreate();
    if (!s_eg) return ESP_ERR_NO_MEM;

    BaseType_t ret = xTaskCreate(campus_net_task, "campus_net", 4096,
                                 NULL, 5, &s_task);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

void campus_net_stop(void)
{
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    if (s_eg)   { vEventGroupDelete(s_eg); s_eg = NULL; }
}

void campus_net_auth_now(void)
{
    if (s_eg) xEventGroupSetBits(s_eg, AUTH_NOW_BIT);
}

bool campus_net_is_online(void) { return s_online; }
