/**
 * @file http_server.c
 * @brief REST API server + app-embedded static file serving.
 */
#include "http_server.h"
#include "system_state.h"
#include "config_manager.h"
#include "campus_net_auth.h"
#include "boot_manager.h"
#include "ota_manager.h"
#include "wifi_manager.h"
#include "log_buffer.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"

#define TAG "http"

static httpd_handle_t    s_server = NULL;
static http_server_deps_t s_deps  = {0};

extern const uint8_t _binary_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t _binary_index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t _binary_style_css_start[]  asm("_binary_style_css_start");
extern const uint8_t _binary_style_css_end[]    asm("_binary_style_css_end");
extern const uint8_t _binary_app_js_start[]     asm("_binary_app_js_start");
extern const uint8_t _binary_app_js_end[]       asm("_binary_app_js_end");

/* ─────────────────────────────────────────────────────────────────── */
/* Auth helper                                                           */
/* ─────────────────────────────────────────────────────────────────── */
static bool check_token(httpd_req_t *req)
{
    char token_stored[64] = {0};
    config_get_str(CFG_KEY_API_TOKEN, token_stored, sizeof(token_stored));

    char auth_hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization",
                                    auth_hdr, sizeof(auth_hdr)) != ESP_OK) {
        return false;
    }
    /* Expect: "Bearer <token>" */
    const char *prefix = "Bearer ";
    if (strncmp(auth_hdr, prefix, strlen(prefix)) != 0) return false;
    return (strcmp(auth_hdr + strlen(prefix), token_stored) == 0);
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"RemoteBoot\"");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t send_json_error(httpd_req_t *req, const char *error, esp_err_t detail)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", error);
    if (detail != ESP_OK) {
        cJSON_AddStringToObject(root, "detail", esp_err_to_name(detail));
    }
    char *json = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, json);
    free(json);
    cJSON_Delete(root);
    return err;
}

static size_t embedded_len(const uint8_t *start, const uint8_t *end)
{
    size_t len = (size_t)(end - start);
    if (len > 0 && end[-1] == '\0') {
        len--;
    }
    return len;
}

static esp_err_t serve_embedded_file(httpd_req_t *req,
                                     const uint8_t *start,
                                     const uint8_t *end,
                                     const char *content_type)
{
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)start, (int)embedded_len(start, end));
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
}

static void schedule_restart(void)
{
    xTaskCreate(restart_task, "esp_restart", 2048, NULL, 5, NULL);
}

/* ─────────────────────────────────────────────────────────────────── */
/* GET /api/status                                                       */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_status(httpd_req_t *req)
{
    system_state_t st;
    system_state_get(&st);

    char ip[16] = {0};
    wifi_manager_get_ip(ip, sizeof(ip));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "pc_state",
                            system_state_pc_str(st.pc_state));
    cJSON_AddStringToObject(root, "campus_net",
                            system_state_campus_str(st.campus_net));
    const char *os_str[] = {"unknown", "windows", "fedora"};
    cJSON_AddStringToObject(root, "os_booted",
                            os_str[st.os_booted < 3 ? st.os_booted : 0]);
    cJSON_AddNumberToObject(root, "last_auth_ts", (double)st.last_auth_ts);
    cJSON_AddStringToObject(root, "esp_ip", ip);
    if (st.pc_state == PC_STATE_ERROR) {
        cJSON_AddStringToObject(root, "error", st.error_msg);
    }

    char *json = cJSON_PrintUnformatted(root);
    send_json(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ─────────────────────────────────────────────────────────────────── */
/* Helpers to read request body into buffer                              */
/* ─────────────────────────────────────────────────────────────────── */
static int read_body(httpd_req_t *req, char *buf, size_t maxlen)
{
    size_t remaining = req->content_len;
    if (remaining >= maxlen) remaining = maxlen - 1;
    int received = httpd_req_recv(req, buf, remaining);
    if (received > 0) buf[received] = '\0';
    return received;
}

/* ─────────────────────────────────────────────────────────────────── */
/* POST /api/boot                                                        */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_boot(httpd_req_t *req)
{
    if (!check_token(req)) return send_unauthorized(req);

    char body[128] = {0};
    read_body(req, body, sizeof(body));

    os_target_t target = OS_TARGET_DEFAULT;
    cJSON *json = cJSON_Parse(body);
    if (json) {
        cJSON *os_item = cJSON_GetObjectItem(json, "os");
        if (os_item && cJSON_IsString(os_item)) {
            if (strcmp(os_item->valuestring, "windows") == 0)
                target = OS_TARGET_WINDOWS;
            else if (strcmp(os_item->valuestring, "fedora") == 0)
                target = OS_TARGET_FEDORA;
            else if (strcmp(os_item->valuestring, "bios") == 0)
                target = OS_TARGET_BIOS;
        }
        cJSON_Delete(json);
    }

    esp_err_t err = boot_manager_boot(s_deps.boot_mgr, target);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"error\":\"pc_not_offline\"}");
    }
    return send_json(req, "{\"status\":\"booting\"}");
}

/* ─────────────────────────────────────────────────────────────────── */
/* POST /api/shutdown                                                    */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_shutdown(httpd_req_t *req)
{
    if (!check_token(req)) return send_unauthorized(req);
    esp_err_t err = boot_manager_shutdown(s_deps.boot_mgr);
    if (err == ESP_ERR_INVALID_STATE)
        return send_json(req, "{\"error\":\"pc_not_online\"}");
    return send_json(req, "{\"status\":\"shutting_down\"}");
}

/* ─────────────────────────────────────────────────────────────────── */
/* POST /api/force_off                                                   */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_force_off(httpd_req_t *req)
{
    if (!check_token(req)) return send_unauthorized(req);
    boot_manager_force_off(s_deps.boot_mgr);
    return send_json(req, "{\"status\":\"forced_off\"}");
}

/* ─────────────────────────────────────────────────────────────────── */
/* POST /api/reboot                                                      */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_reboot(httpd_req_t *req)
{
    if (!check_token(req)) return send_unauthorized(req);
    boot_manager_reset(s_deps.boot_mgr);
    system_state_set_pc(PC_STATE_POST_RUNNING);
    return send_json(req, "{\"status\":\"rebooting\"}");
}

/* ─────────────────────────────────────────────────────────────────── */
/* POST /api/reboot_os                                                   */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_reboot_os(httpd_req_t *req)
{
    if (!check_token(req)) return send_unauthorized(req);
    char body[128] = {0};
    read_body(req, body, sizeof(body));

    os_target_t target = OS_TARGET_DEFAULT;
    cJSON *json = cJSON_Parse(body);
    if (json) {
        cJSON *os_item = cJSON_GetObjectItem(json, "os");
        if (os_item && cJSON_IsString(os_item)) {
            if (strcmp(os_item->valuestring, "windows") == 0)
                target = OS_TARGET_WINDOWS;
            else if (strcmp(os_item->valuestring, "fedora") == 0)
                target = OS_TARGET_FEDORA;
            else if (strcmp(os_item->valuestring, "bios") == 0)
                target = OS_TARGET_BIOS;
        }
        cJSON_Delete(json);
    }
    boot_manager_reboot_os(s_deps.boot_mgr, target);
    return send_json(req, "{\"status\":\"rebooting\"}");
}

/* ─────────────────────────────────────────────────────────────────── */
/* GET /api/config                                                       */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_config_get(httpd_req_t *req)
{
    if (!check_token(req)) return send_unauthorized(req);

    char ssid[64]={0}, pc_ip[32]={0};
    config_get_str(CFG_KEY_WIFI_SSID, ssid, sizeof(ssid));
    config_get_str(CFG_KEY_PC_IP,     pc_ip, sizeof(pc_ip));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi_ssid",    ssid);
    cJSON_AddStringToObject(root, "wifi_pass",    "***");
    cJSON_AddStringToObject(root, "campus_pass",  "***");
    cJSON_AddStringToObject(root, "pc_ip",        pc_ip);
    cJSON_AddNumberToObject(root, "grub_win_idx",
        config_get_u8(CFG_KEY_GRUB_WIN_IDX, 0));
    cJSON_AddNumberToObject(root, "grub_fed_idx",
        config_get_u8(CFG_KEY_GRUB_FED_IDX, 1));
    cJSON_AddNumberToObject(root, "grub_default",
        config_get_u8(CFG_KEY_GRUB_DEFAULT, 0));
    cJSON_AddNumberToObject(root, "grub_wait_ms",
        config_get_u16(CFG_KEY_GRUB_WAIT_MS, 500));
    cJSON_AddNumberToObject(root, "ping_interval_s",
        config_get_u16(CFG_KEY_PING_INTERVAL_S, 300));
    cJSON_AddNumberToObject(root, "hdd_quiet_ms",
        config_get_u16(CFG_KEY_HDD_QUIET_MS, 1500));
    cJSON_AddNumberToObject(root, "relay_pol",
        config_get_u8(CFG_KEY_RELAY_POLARITY, 0));
    cJSON_AddNumberToObject(root, "btldr_type",
        config_get_u8(CFG_KEY_BOOTLOADER_TYPE, 1));
    cJSON_AddNumberToObject(root, "post_settle",
        config_get_u16(CFG_KEY_POST_SETTLE_MS, CONFIG_RB_USB_POST_SETTLE_MS));

    char *js = cJSON_PrintUnformatted(root);
    send_json(req, js);
    free(js); cJSON_Delete(root);
    return ESP_OK;
}

/* ─────────────────────────────────────────────────────────────────── */
/* PUT /api/config                                                       */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_config_put(httpd_req_t *req)
{
    if (!check_token(req)) return send_unauthorized(req);

    char body[512] = {0};
    read_body(req, body, sizeof(body));

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"error\":\"invalid_json\"}");
    }

#define SET_STR(key, nvs_key) do { \
    cJSON *it = cJSON_GetObjectItem(json, key); \
    if (it && cJSON_IsString(it) && it->valuestring[0]) \
        config_set_str(nvs_key, it->valuestring); \
} while(0)

#define SET_U8(key, nvs_key) do { \
    cJSON *it = cJSON_GetObjectItem(json, key); \
    if (it && cJSON_IsNumber(it)) \
        config_set_u8(nvs_key, (uint8_t)it->valuedouble); \
} while(0)

#define SET_U16(key, nvs_key) do { \
    cJSON *it = cJSON_GetObjectItem(json, key); \
    if (it && cJSON_IsNumber(it)) \
        config_set_u16(nvs_key, (uint16_t)it->valuedouble); \
} while(0)

    SET_STR("wifi_ssid",      CFG_KEY_WIFI_SSID);
    SET_STR("wifi_pass",      CFG_KEY_WIFI_PASS);
    SET_STR("campus_card",    CFG_KEY_CAMPUS_CARD);
    SET_STR("campus_pass",    CFG_KEY_CAMPUS_PASS);
    SET_STR("pc_ip",          CFG_KEY_PC_IP);
    SET_U8 ("grub_win_idx",   CFG_KEY_GRUB_WIN_IDX);
    SET_U8 ("grub_fed_idx",   CFG_KEY_GRUB_FED_IDX);
    SET_U8 ("grub_default",   CFG_KEY_GRUB_DEFAULT);
    SET_U16("grub_wait_ms",   CFG_KEY_GRUB_WAIT_MS);
    SET_U16("ping_interval_s",CFG_KEY_PING_INTERVAL_S);
    SET_U16("hdd_quiet_ms",   CFG_KEY_HDD_QUIET_MS);
    SET_U8 ("relay_pol",      CFG_KEY_RELAY_POLARITY);
    SET_U8 ("btldr_type",     CFG_KEY_BOOTLOADER_TYPE);
    SET_U16("post_settle",    CFG_KEY_POST_SETTLE_MS);

    cJSON_Delete(json);
    return send_json(req, "{\"status\":\"saved\"}");
}

/* ─────────────────────────────────────────────────────────────────── */
/* GET /api/netstat                                                      */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_netstat(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "online", campus_net_is_online());
    system_state_t st; system_state_get(&st);
    cJSON_AddStringToObject(root, "campus_net",
                            system_state_campus_str(st.campus_net));
    cJSON_AddNumberToObject(root, "last_auth_ts", (double)st.last_auth_ts);
    char *js = cJSON_PrintUnformatted(root);
    send_json(req, js);
    free(js); cJSON_Delete(root);
    return ESP_OK;
}

/* ─────────────────────────────────────────────────────────────────── */
/* GET /api/log                                                          */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_log(httpd_req_t *req)
{
    if (!check_token(req)) return send_unauthorized(req);

    /* Parse query params from URI */
    char uri[256] = {0};
    int start = 0, count = 100, clear_flag = 0, info_flag = 0;
    if (httpd_req_get_url_query_str(req, uri, sizeof(uri)) == ESP_OK) {
        char val[32];
        if (httpd_query_key_value(uri, "start", val, sizeof(val)) == ESP_OK)
            start = atoi(val);
        if (httpd_query_key_value(uri, "count", val, sizeof(val)) == ESP_OK)
            count = atoi(val);
        if (count > 500) count = 500;
        if (httpd_query_key_value(uri, "clear", val, sizeof(val)) == ESP_OK)
            clear_flag = atoi(val);
        if (httpd_query_key_value(uri, "info", val, sizeof(val)) == ESP_OK)
            info_flag = atoi(val);
    }

    if (clear_flag) {
        log_buffer_clear();
    }

    int capacity, total, head;
    bool wrapped;
    log_buffer_info(&capacity, &total, &head, &wrapped);

    if (info_flag) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "capacity", capacity);
        cJSON_AddNumberToObject(root, "count", total);
        cJSON_AddNumberToObject(root, "head", head);
        cJSON_AddBoolToObject(root, "wrapped", wrapped);
        char *js = cJSON_PrintUnformatted(root);
        send_json(req, js);
        free(js); cJSON_Delete(root);
        return ESP_OK;
    }

    /* Read requested range */
    if (start > total) start = total > 0 ? total - 1 : 0;
    int want = count;
    if (start + want > total) want = total - start;
    if (want < 0) want = 0;

    log_entry_t *entries = malloc((size_t)want * sizeof(log_entry_t));
    int got = 0;
    if (entries && want > 0) {
        got = log_buffer_read(start, want, entries);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "capacity", capacity);
    cJSON_AddNumberToObject(root, "count", total);
    cJSON_AddNumberToObject(root, "head", head);
    cJSON_AddNumberToObject(root, "start", start);
    cJSON_AddNumberToObject(root, "returned", got);

    cJSON *arr = cJSON_AddArrayToObject(root, "entries");
    for (int i = 0; i < got; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "ts",  (double)entries[i].timestamp_ms);
        cJSON_AddNumberToObject(e, "lvl", entries[i].level);
        cJSON_AddStringToObject(e, "tag", entries[i].tag);
        cJSON_AddStringToObject(e, "msg", entries[i].msg);
        cJSON_AddItemToArray(arr, e);
    }

    char *js = cJSON_PrintUnformatted(root);
    send_json(req, js);
    free(js); cJSON_Delete(root);
    free(entries);
    return ESP_OK;
}

/* ─────────────────────────────────────────────────────────────────── */
/* GET /api/update/info                                                 */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_update_info(httpd_req_t *req)
{
    if (!check_token(req)) return send_unauthorized(req);

    ota_runtime_info_t info;
    esp_err_t err = ota_manager_get_runtime_info(&info);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json_error(req, "ota_info_failed", err);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "update_supported", info.update_supported);
    cJSON_AddBoolToObject(root, "rollback_pending", info.rollback_pending);
    cJSON_AddStringToObject(root, "project_name", info.project_name);
    cJSON_AddStringToObject(root, "version", info.version);
    cJSON_AddStringToObject(root, "idf_ver", info.idf_ver);
    cJSON_AddStringToObject(root, "build_date", info.build_date);
    cJSON_AddStringToObject(root, "build_time", info.build_time);
    cJSON_AddStringToObject(root, "running_partition", info.running_partition);
    cJSON_AddStringToObject(root, "boot_partition", info.boot_partition);
    cJSON_AddStringToObject(root, "next_partition", info.next_partition);
    cJSON_AddStringToObject(root, "ota_state", info.ota_state);

    char *json = cJSON_PrintUnformatted(root);
    send_json(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ─────────────────────────────────────────────────────────────────── */
/* POST /api/update                                                     */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_update(httpd_req_t *req)
{
    if (!check_token(req)) return send_unauthorized(req);

    if (req->content_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json_error(req, "empty_firmware", ESP_ERR_INVALID_SIZE);
    }

    char content_type[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type",
                                    content_type, sizeof(content_type)) == ESP_OK) {
        if (strncmp(content_type, "application/octet-stream", 24) != 0) {
            httpd_resp_set_status(req, "415 Unsupported Media Type");
            return send_json_error(req, "unsupported_content_type", ESP_ERR_INVALID_ARG);
        }
    }

    ota_session_t session;
    esp_err_t err = ota_manager_begin(&session, (size_t)req->content_len);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json_error(req, "ota_busy", err);
    }
    if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json_error(req, "ota_pending_verify", err);
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json_error(req, "ota_begin_failed", err);
    }

    uint8_t buf[4096];
    int remaining = req->content_len;

    while (remaining > 0) {
        int want = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int received = httpd_req_recv(req, (char *)buf, want);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            ota_manager_abort(&session);
            httpd_resp_set_status(req, "400 Bad Request");
            return send_json_error(req, "upload_interrupted", ESP_FAIL);
        }

        err = ota_manager_write(&session, buf, (size_t)received);
        if (err != ESP_OK) {
            ota_manager_abort(&session);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return send_json_error(req, "ota_write_failed", err);
        }

        remaining -= received;
    }

    err = ota_manager_finish(&session);
    if (err != ESP_OK) {
        httpd_resp_set_status(req,
            err == ESP_ERR_OTA_VALIDATE_FAILED ? "400 Bad Request" : "500 Internal Server Error");
        return send_json_error(req, "ota_finalize_failed", err);
    }

    err = send_json(req, "{\"status\":\"uploaded\",\"rebooting\":true}");
    if (err == ESP_OK) {
        schedule_restart();
    }
    return err;
}

/* ─────────────────────────────────────────────────────────────────── */
/* POST /api/reset_state                                                  */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_reset_state(httpd_req_t *req)
{
    if (!check_token(req)) return send_unauthorized(req);
    system_state_set_pc(PC_STATE_OFFLINE);
    system_state_set_os(BOOTED_OS_UNKNOWN);
    ESP_LOGI(TAG, "State reset to OFFLINE via API");
    return send_json(req, "{\"status\":\"reset_to_offline\"}");
}

/* ─────────────────────────────────────────────────────────────────── */
/* POST /api/auth_now                                                    */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_auth_now(httpd_req_t *req)
{
    if (!check_token(req)) return send_unauthorized(req);
    campus_net_auth_now();
    return send_json(req, "{\"status\":\"triggered\"}");
}

/* ─────────────────────────────────────────────────────────────────── */
/* CORS preflight (OPTIONS *)                                            */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,PUT,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization,Content-Type");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

/* ─────────────────────────────────────────────────────────────────── */
/* Serve static files embedded in the app image                         */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t handler_root(httpd_req_t *req)
{
    return serve_embedded_file(req,
                               _binary_index_html_start,
                               _binary_index_html_end,
                               "text/html; charset=utf-8");
}

static esp_err_t handler_style(httpd_req_t *req)
{
    return serve_embedded_file(req,
                               _binary_style_css_start,
                               _binary_style_css_end,
                               "text/css; charset=utf-8");
}

static esp_err_t handler_app(httpd_req_t *req)
{
    return serve_embedded_file(req,
                               _binary_app_js_start,
                               _binary_app_js_end,
                               "application/javascript; charset=utf-8");
}

/* ─────────────────────────────────────────────────────────────────── */

#define REGISTER(_method, _uri, _fn) do { \
    httpd_uri_t h = { .uri=(_uri), .method=(_method), .handler=(_fn), .user_ctx=NULL }; \
    httpd_register_uri_handler(s_server, &h); \
} while(0)

esp_err_t http_server_start(const http_server_deps_t *deps)
{
    s_deps = *deps;

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 36;
    cfg.stack_size       = 12288;  /* OTA handler's 4KB buf needs extra room */

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) return err;

    /* Static web UI */
    REGISTER(HTTP_GET,  "/",              handler_root);
    REGISTER(HTTP_GET,  "/style.css",     handler_style);
    REGISTER(HTTP_GET,  "/app.js",        handler_app);

    /* REST API */
    REGISTER(HTTP_GET,  "/api/status",    handler_status);
    REGISTER(HTTP_POST, "/api/boot",      handler_boot);
    REGISTER(HTTP_POST, "/api/shutdown",  handler_shutdown);
    REGISTER(HTTP_POST, "/api/force_off", handler_force_off);
    REGISTER(HTTP_POST, "/api/reboot",    handler_reboot);
    REGISTER(HTTP_POST, "/api/reboot_os", handler_reboot_os);
    REGISTER(HTTP_GET,  "/api/update/info", handler_update_info);
    REGISTER(HTTP_POST, "/api/update",      handler_update);
    REGISTER(HTTP_GET,  "/api/config",    handler_config_get);
    REGISTER(HTTP_PUT,  "/api/config",    handler_config_put);
    REGISTER(HTTP_GET,  "/api/netstat",   handler_netstat);
    REGISTER(HTTP_GET,  "/api/log",       handler_log);
    REGISTER(HTTP_POST, "/api/reset_state", handler_reset_state);
    REGISTER(HTTP_POST, "/api/auth_now",  handler_auth_now);

    /* CORS preflights */
    REGISTER(HTTP_OPTIONS, "/api/boot",      handler_options);
    REGISTER(HTTP_OPTIONS, "/api/config",    handler_options);
    REGISTER(HTTP_OPTIONS, "/api/shutdown",  handler_options);
    REGISTER(HTTP_OPTIONS, "/api/force_off", handler_options);
    REGISTER(HTTP_OPTIONS, "/api/reboot",    handler_options);
    REGISTER(HTTP_OPTIONS, "/api/reboot_os", handler_options);
    REGISTER(HTTP_OPTIONS, "/api/update/info", handler_options);
    REGISTER(HTTP_OPTIONS, "/api/update",      handler_options);
    REGISTER(HTTP_OPTIONS, "/api/reset_state", handler_options);
    REGISTER(HTTP_OPTIONS, "/api/auth_now",  handler_options);
    REGISTER(HTTP_OPTIONS, "/api/log",       handler_options);

    ESP_LOGI(TAG, "HTTP server started on port %d", cfg.server_port);
    return ESP_OK;
}

void http_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
