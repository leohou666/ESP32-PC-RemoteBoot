/**
 * @file http_server.c
 * @brief REST API server + SPIFFS static file serving.
 */
#include "http_server.h"
#include "system_state.h"
#include "config_manager.h"
#include "campus_net_auth.h"
#include "boot_manager.h"
#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "cJSON.h"

#define TAG "http"

static httpd_handle_t    s_server = NULL;
static http_server_deps_t s_deps  = {0};

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
/* Serve static files from SPIFFS                                       */
/* ─────────────────────────────────────────────────────────────────── */
static esp_err_t serve_file(httpd_req_t *req, const char *filepath, const char *content_type)
{
    FILE *f = fopen(filepath, "r");
    if (!f) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "File not found. Flash SPIFFS image.");
        return ESP_OK;
    }
    httpd_resp_set_type(req, content_type);
    /* For JS/CSS, add cache controls to improve load times */
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");

    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, n);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handler_root(httpd_req_t *req)  { return serve_file(req, "/spiffs/index.html", "text/html"); }
static esp_err_t handler_style(httpd_req_t *req) { return serve_file(req, "/spiffs/style.css", "text/css"); }
static esp_err_t handler_app(httpd_req_t *req)   { return serve_file(req, "/spiffs/app.js", "application/javascript"); }

/* ─────────────────────────────────────────────────────────────────── */

#define REGISTER(_method, _uri, _fn) do { \
    httpd_uri_t h = { .uri=(_uri), .method=(_method), .handler=(_fn), .user_ctx=NULL }; \
    httpd_register_uri_handler(s_server, &h); \
} while(0)

esp_err_t http_server_start(const http_server_deps_t *deps)
{
    s_deps = *deps;

    /* Mount SPIFFS */
    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path       = "/spiffs",
        .partition_label = "spiffs",
        .max_files       = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&spiffs_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s (web UI unavailable)", esp_err_to_name(err));
    }

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 32;

    err = httpd_start(&s_server, &cfg);
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
    REGISTER(HTTP_GET,  "/api/config",    handler_config_get);
    REGISTER(HTTP_PUT,  "/api/config",    handler_config_put);
    REGISTER(HTTP_GET,  "/api/netstat",   handler_netstat);
    REGISTER(HTTP_POST, "/api/auth_now",  handler_auth_now);

    /* CORS preflights */
    REGISTER(HTTP_OPTIONS, "/api/boot",      handler_options);
    REGISTER(HTTP_OPTIONS, "/api/config",    handler_options);
    REGISTER(HTTP_OPTIONS, "/api/shutdown",  handler_options);
    REGISTER(HTTP_OPTIONS, "/api/force_off", handler_options);
    REGISTER(HTTP_OPTIONS, "/api/reboot",    handler_options);
    REGISTER(HTTP_OPTIONS, "/api/reboot_os", handler_options);
    REGISTER(HTTP_OPTIONS, "/api/auth_now",  handler_options);

    ESP_LOGI(TAG, "HTTP server started on port %d", cfg.server_port);
    return ESP_OK;
}

void http_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_vfs_spiffs_unregister("spiffs");
}
