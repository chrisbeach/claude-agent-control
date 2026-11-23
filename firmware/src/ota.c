#include "ota.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "mdns.h"
#include "board.h"

static const char *TAG = "ota";

#define CHUNK 4096
#define RECV_TIMEOUT_RETRIES 5

static const char *s_bearer;
static const char *s_hostname;
static volatile bool s_in_progress;
static esp_timer_handle_t s_rollback_timer;
static esp_timer_handle_t s_restart_timer;
static httpd_handle_t s_server;

static const char *state_name(void) {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK) return "unknown";
    switch (state) {
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_VALID: return "valid";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted";
        default: return "undefined"; /* flashed over USB, never went through OTA */
    }
}

static void sha_hex(const uint8_t *sha, char *out, size_t out_len) {
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    for (size_t i = 0; i < 32 && n + 2 < out_len; i++) {
        out[n++] = hex[sha[i] >> 4];
        out[n++] = hex[sha[i] & 0xf];
    }
    out[n] = '\0';
}

static const char *reset_reason_name(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "wdt";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        default: return "other";
    }
}

bool ota_authorised(httpd_req_t *req) {
    char header[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) return false;
    if (strncmp(header, "Bearer ", 7) != 0) return false;
    return strcmp(header + 7, s_bearer) == 0;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *json) {
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return httpd_resp_send_500(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *detail) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", detail);
    return send_json(req, status, json);
}

static esp_err_t status_handler(httpd_req_t *req) {
    const esp_app_desc_t *app = esp_app_get_description();
    char sha[65];
    sha_hex(app->app_elf_sha256, sha, sizeof(sha));

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "hostname", s_hostname);
    cJSON_AddStringToObject(json, "project", app->project_name);
    cJSON_AddStringToObject(json, "version", app->version);
    cJSON_AddStringToObject(json, "built", app->date);
    cJSON_AddStringToObject(json, "built_time", app->time);
    cJSON_AddStringToObject(json, "idf", app->idf_ver);
    cJSON_AddStringToObject(json, "elf_sha256", sha);
    cJSON_AddStringToObject(json, "slot", esp_ota_get_running_partition()->label);
    cJSON_AddStringToObject(json, "ota_state", state_name());
    cJSON_AddStringToObject(json, "reset_reason", reset_reason_name());
    cJSON_AddNumberToObject(json, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(json, "free_heap", (double)esp_get_free_heap_size());
    /* internal RAM alone (no PSRAM): what Wi-Fi, lwIP, task stacks and every other small malloc live in */
    cJSON_AddNumberToObject(json, "free_internal", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(json, "largest_internal", (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(json, "min_free_internal", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    /* input diagnostics: the backlight schedule keeps the panel lit while idle_ms < 60000 */
    cJSON_AddBoolToObject(json, "touch", board_touch_available());
    lv_display_t *disp = lv_display_get_default();
    if (disp) cJSON_AddNumberToObject(json, "idle_ms", (double)lv_display_get_inactive_time(disp));
    lv_indev_t *indev = lv_indev_get_next(NULL);
    if (indev) {
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        cJSON_AddBoolToObject(json, "pressed", lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED);
        cJSON_AddNumberToObject(json, "touch_x", pt.x);
        cJSON_AddNumberToObject(json, "touch_y", pt.y);
    }
    cJSON_AddBoolToObject(json, "update_in_progress", s_in_progress);

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
        char text[16];
        snprintf(text, sizeof(text), IPSTR, IP2STR(&ip.ip));
        cJSON_AddStringToObject(json, "ip", text);
    }
    return send_json(req, "200 OK", json);
}

static void restart_cb(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "restarting into the new image");
    esp_restart();
}

static esp_err_t ota_handler(httpd_req_t *req) {
    if (!ota_authorised(req)) return send_error(req, "401 Unauthorized", "bad or missing bearer token");
    if (s_in_progress) return send_error(req, "409 Conflict", "an update is already in progress");

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) return send_error(req, "500 Internal Server Error", "no OTA partition");
    if (req->content_len <= 0) return send_error(req, "400 Bad Request", "empty body");
    if ((uint32_t)req->content_len > target->size) return send_error(req, "413 Payload Too Large", "image larger than the slot");

    char *buf = malloc(CHUNK);
    if (!buf) return send_error(req, "500 Internal Server Error", "out of memory");

    s_in_progress = true;
    ESP_LOGI(TAG, "receiving %d bytes into %s", req->content_len, target->label);
    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    size_t received = 0;
    int timeouts = 0;
    while (err == ESP_OK && received < (size_t)req->content_len) {
        int n = httpd_req_recv(req, buf, CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= RECV_TIMEOUT_RETRIES) continue;
        if (n <= 0) {
            ESP_LOGW(TAG, "receive failed at %u bytes (%d)", (unsigned)received, n);
            err = ESP_FAIL;
            break;
        }
        timeouts = 0;
        err = esp_ota_write(handle, buf, n);
        received += n;
    }
    free(buf);

    if (err == ESP_OK) err = esp_ota_end(handle);
    else if (handle) esp_ota_abort(handle);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(target);
    s_in_progress = false;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
        const char *status = err == ESP_ERR_OTA_VALIDATE_FAILED ? "400 Bad Request" : "500 Internal Server Error";
        return send_error(req, status, esp_err_to_name(err));
    }

    esp_app_desc_t next;
    char sha[65] = "";
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "slot", target->label);
    cJSON_AddNumberToObject(json, "bytes", (double)received);
    if (esp_ota_get_partition_description(target, &next) == ESP_OK) {
        sha_hex(next.app_elf_sha256, sha, sizeof(sha));
        cJSON_AddStringToObject(json, "version", next.version);
        cJSON_AddStringToObject(json, "elf_sha256", sha);
    }
    ESP_LOGI(TAG, "wrote %u bytes to %s (%s); rebooting", (unsigned)received, target->label, next.version);
    esp_err_t sent = send_json(req, "200 OK", json);
    esp_timer_start_once(s_restart_timer, 700 * 1000); /* let the reply leave first */
    return sent;
}

static void rollback_cb(void *arg) {
    (void)arg;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGE(TAG, "new image never confirmed itself; rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void ota_confirm(void) {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;
    ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
    if (s_rollback_timer) esp_timer_stop(s_rollback_timer);
    ESP_LOGI(TAG, "new image confirmed (%s)", esp_ota_get_running_partition()->label);
}

esp_err_t ota_start(const char *bearer, const char *hostname, uint32_t confirm_timeout_s) {
    s_bearer = bearer;
    s_hostname = hostname;

    const esp_timer_create_args_t restart_args = {.callback = restart_cb, .name = "ota_restart"};
    ESP_ERROR_CHECK(esp_timer_create(&restart_args, &s_restart_timer));
    const esp_timer_create_args_t rollback_args = {.callback = rollback_cb, .name = "ota_rollback"};
    ESP_ERROR_CHECK(esp_timer_create(&rollback_args, &s_rollback_timer));
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "running an unconfirmed image; %u s to confirm itself or roll back", (unsigned)confirm_timeout_s);
        ESP_ERROR_CHECK(esp_timer_start_once(s_rollback_timer, (uint64_t)confirm_timeout_s * 1000000));
    }

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(hostname));
    mdns_instance_name_set("Agent Control display");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8 * 1024;
    cfg.recv_wait_timeout = 15;
    cfg.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) return err;
    s_server = server;

    const httpd_uri_t status_uri = {.uri = "/status", .method = HTTP_GET, .handler = status_handler};
    const httpd_uri_t ota_uri = {.uri = "/ota", .method = HTTP_POST, .handler = ota_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota_uri));
    ESP_LOGI(TAG, "listening on http://%s.local/ (status, ota)", hostname);
    return ESP_OK;
}

httpd_handle_t ota_httpd(void) { return s_server; }
