/* Agent Control: every live Claude Code session from the agent-control hub, on a
 * Waveshare ESP32-S3-Touch-LCD-7. The device holds only the hub's device key. */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ac_settings.h" /* secrets.ini, as settings_header.py wrote it out for this build */
#include "board.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "fetch.h"
#include "fleet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "leds.h"
#include "ota.h"
#include "ui.h"
#include "wifi.h"

static const char *TAG = "main";

#define LONG_POLL_SECONDS 25           /* the hub holds the request this long when nothing changes */
#define LONG_POLL_TIMEOUT_MS ((LONG_POLL_SECONDS + 15) * 1000)
#define RETRY_SECONDS 3                /* after a failed request */
#define LINK_LOST_SECONDS 20           /* this long without an answer and the list is withdrawn */
#define MAX_FLEET_BODY (24 * 1024)
#define QUIET_MS (30 * 60 * 1000)      /* nothing working, waiting or done for this long: backlight off ... */
#define WAKE_MS 60000                  /* ... until something is, or for a minute after a touch */
#define OTA_CONFIRM_SECONDS 600        /* a new image must fetch the fleet within this or it is rolled back */
#define SCREENSHOT_ROWS 8              /* rows per chunk of GET /screenshot */
#define KEY_LEN sizeof(((ac_session_t *)0)->key)
#define MAX_ANSWER_BODY 1024

extern const uint8_t antonio_start[] asm("_binary_Antonio_Bold_ttf_start");
extern const uint8_t antonio_end[] asm("_binary_Antonio_Bold_ttf_end");

static ac_model_t fresh; /* ~7 KB: static, never on a task stack */
static char rev[sizeof(fresh.rev)];
static bool have_model;
static int64_t last_good_us;
static fetch_session_t session = {.bearer = AC_DEVICE_KEY};
static bool backlight_on;
static bool screen_off; /* POST /screen?on=0: dark and deaf to touch until on=1, or a restart */
static QueueHandle_t request_queue; /* taps waiting to be sent to the hub: a session's key, or "" for the "+" */

/* Lit while any session is working, needs input or is done (its result is waiting to
 * be looked at), and for QUIET_MS after the last of those has gone; dark once there
 * is nothing but unused sessions, or none. A touch wakes it for WAKE_MS. All of that only while
 * the screen is switched on: after POST /screen?on=0 it is dark whatever the sessions do. */
static void backlight_timer(lv_timer_t *timer) {
    (void)timer;
    static uint32_t last_attention; /* lv ticks; boot counts as activity */
    if (ui_attention()) last_attention = lv_tick_get();
    uint32_t idle = lv_display_get_inactive_time(board_display());
    bool want = !screen_off && (lv_tick_elaps(last_attention) < QUIET_MS || idle < WAKE_MS);
    if (want != backlight_on) {
        ESP_LOGI(TAG, "backlight %s", want ? "on" : "off");
        board_backlight(want);
        backlight_on = want;
    }
}

/* The optional LEDs, left to right as the header's figures: WORKING, NEEDS INPUT, DONE, each
 * lit in its state's colour while any session is in it, and pulsing as that state's pill does.
 * DONE lights only for a result with something left to do: a quiet (dimmed) row does not count.
 * Dark while the list is withdrawn. */
static void show_leds(const ac_model_t *model) {
    static const ac_status_t BAND[3] = {AC_WORKING, AC_WAITING, AC_DONE};
    int count[3] = {model ? model->working : 0, model ? model->waiting : 0, ac_done_to_check(model)};
    uint32_t colours[3];
    uint8_t pulsing = 0;
    for (int i = 0; i < 3; i++) {
        colours[i] = count[i] > 0 ? ui_status_colour(BAND[i]) : 0;
        if (count[i] > 0 && ui_status_pulses(BAND[i])) pulsing |= 1 << i;
    }
    leds_set(colours, pulsing);
}

static void show_link(ui_link_t link, const char *detail) {
    if (link != UI_LINK_OK) show_leds(NULL);
    lvgl_port_lock(0);
    ui_set_link(link, detail);
    lvgl_port_unlock();
}

static void show_fetch_failure(void) {
    if (!have_model) show_link(UI_LINK_FETCHING, NULL);
    else if (esp_timer_get_time() - last_good_us > (int64_t)LINK_LOST_SECONDS * 1000000) show_link(UI_LINK_LOST, NULL);
}

/* One long poll: returns true when the hub answered (changed or not). With a rev in
 * hand the hub holds the request until the fleet changes or LONG_POLL_SECONDS
 * pass; the first call (no rev) answers at once. */
static bool fetch_once(void) {
    char url[224];
    if (have_model && rev[0]) snprintf(url, sizeof(url), "%s/api/fleet?rev=%s&wait=%d", AC_HUB_URL, rev, LONG_POLL_SECONDS);
    else snprintf(url, sizeof(url), "%s/api/fleet", AC_HUB_URL);
    fetch_result_t result;
    if (fetch_session_get(&session, url, MAX_FLEET_BODY, LONG_POLL_TIMEOUT_MS, &result) != ESP_OK) {
        show_fetch_failure();
        return false;
    }

    char problem[96] = "";
    bool ok = result.status == 200 && ac_fleet_parse(result.body, result.length, &fresh, problem, sizeof(problem));
    free(result.body);
    if (!ok) {
        ESP_LOGW(TAG, "HTTP %d: %s", result.status, problem);
        char detail[48];
        if (result.status == 401) snprintf(detail, sizeof(detail), "WRONG DEVICE KEY");
        else if (result.status != 200) snprintf(detail, sizeof(detail), "HUB ANSWERED %d", result.status);
        else snprintf(detail, sizeof(detail), "UNREADABLE ANSWER");
        show_link(UI_LINK_ERROR, detail);
        return false;
    }

    snprintf(rev, sizeof(rev), "%s", fresh.rev);
    have_model = true;
    last_good_us = esp_timer_get_time();
    ota_confirm(); /* the new build works end to end: keep it */
    lvgl_port_lock(0);
    ui_set_model(&fresh); /* also when unchanged: every answer resets the screen's clock */
    lvgl_port_unlock();
    show_leds(&fresh);
    return true;
}

static void fetch_task(void *arg) {
    (void)arg;
    bool was_connected = false;
    while (true) {
        if (!wifi_connected()) {
            if (was_connected) {
                fetch_session_close(&session);
                show_link(have_model ? UI_LINK_LOST : UI_LINK_WIFI, have_model ? NULL : AC_WIFI_SSID);
            }
            was_connected = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!was_connected) {
            was_connected = true;
            if (!have_model) show_link(UI_LINK_FETCHING, NULL);
        }
        bool ok = fetch_once();
        vTaskDelay(pdMS_TO_TICKS(ok ? 100 : RETRY_SECONDS * 1000)); /* straight back into the next long poll */
    }
}

/* What became of a tap's request, on the screen. The caller holds LVGL's lock. */
static void show_result(const char *key, bool ok, const char *message) {
    if (key[0]) ui_set_focus_result(key, ok, message);
    else ui_set_new_result(ok, message);
}

/* A tap, from LVGL's task: hand it over and return. A third while two are still unsent is turned away. */
static void on_request(const char *key) {
    char item[KEY_LEN];
    snprintf(item, sizeof(item), "%s", key);
    if (xQueueSend(request_queue, item, 0) != pdTRUE) show_result(key, false, "STILL BUSY WITH THE LAST TAP");
}

static void on_new(void) { on_request(""); }

/* Asks the hub for what each tap wants (a session's window raised on its PC, or a new session opened)
 * and shows the hub's answer. Its own connection: the long poll's is held open for up to LONG_POLL_SECONDS. */
static void request_task(void *arg) {
    (void)arg;
    char key[KEY_LEN], url[224], message[100];
    while (true) {
        xQueueReceive(request_queue, key, portMAX_DELAY);
        if (key[0]) snprintf(url, sizeof(url), "%s/api/focus?key=%s", AC_HUB_URL, key);
        else snprintf(url, sizeof(url), "%s/api/new", AC_HUB_URL);
        bool ok = false;
        snprintf(message, sizeof(message), "NO ANSWER FROM THE HUB");
        fetch_result_t result;
        if (fetch_post(url, AC_DEVICE_KEY, MAX_ANSWER_BODY, &result) == ESP_OK) {
            cJSON *json = result.status == 200 ? cJSON_ParseWithLength(result.body, result.length) : NULL;
            const cJSON *said = cJSON_GetObjectItemCaseSensitive(json, "message");
            if (cJSON_IsString(said)) {
                ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ok"));
                snprintf(message, sizeof(message), "%s", said->valuestring);
            } else {
                snprintf(message, sizeof(message), "HUB ANSWERED %d", result.status);
            }
            cJSON_Delete(json);
            free(result.body);
        }
        ESP_LOGI(TAG, "%s: %s", key[0] ? key : "new session", message);
        lvgl_port_lock(0);
        show_result(key, ok, message);
        lvgl_port_unlock();
    }
}

static esp_err_t refuse(httpd_req_t *req) {
    httpd_resp_set_status(req, "401 Unauthorized");
    return httpd_resp_sendstr(req, "bad or missing bearer token\n");
}

/* POST /tap?row=N or /tap?new=1 (bearer = the device key): what a finger on row N of the list,
 * or on the "+", does. With /screenshot it lets a build be tried without standing at the panel. */
static esp_err_t tap_handler(httpd_req_t *req) {
    if (!ota_authorised(req)) return refuse(req);
    char query[32], value[8];
    bool have_query = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK;
    bool row = have_query && httpd_query_key_value(query, "row", value, sizeof(value)) == ESP_OK;
    if (!row && !(have_query && httpd_query_key_value(query, "new", value, sizeof(value)) == ESP_OK)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "row=N or new=1 is required\n");
    }
    lvgl_port_lock(0);
    lv_display_trigger_activity(NULL); /* a touch, as far as the backlight goes */
    if (row) ui_tap_row(atoi(value));
    else ui_tap_new();
    lvgl_port_unlock();
    return httpd_resp_sendstr(req, "ok\n");
}

/* POST /screen?on=0|1 (bearer = the device key): 0 switches the panel off, for a "screens off"
 * button that also darkens the room's monitors: backlight and LEDs dark whatever the sessions do,
 * and touches do nothing (a finger on a dark panel must not raise a window or open a session).
 * 1 hands the backlight back to its usual rule, lit for WAKE_MS first as after a touch.
 * GET /screen says which it is. The fleet is still followed meanwhile, so the list is current the
 * moment the panel is lit. Not kept across a restart: the panel always starts switched on. */
static esp_err_t screen_handler(httpd_req_t *req) {
    if (!ota_authorised(req)) return refuse(req);
    if (req->method == HTTP_POST) {
        char query[16], value[4];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, "on", value, sizeof(value)) != ESP_OK ||
            (strcmp(value, "0") != 0 && strcmp(value, "1") != 0)) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "on=0 or on=1 is required\n");
        }
        bool off = value[0] == '0';
        lvgl_port_lock(0);
        screen_off = off;
        lv_indev_reset(NULL, NULL); /* a press under way is dropped, not finished later */
        lv_indev_enable(NULL, !off);
        if (!off) lv_display_trigger_activity(NULL);
        backlight_timer(NULL); /* now, not at its next tick */
        lvgl_port_unlock();
        leds_dark(off);
        ESP_LOGI(TAG, "screen %s", off ? "off" : "on");
    }
    return httpd_resp_sendstr(req, screen_off ? "off\n" : "on\n");
}

/* GET /screenshot (bearer = the device key): the screen as LVGL draws it, raw
 * RGB565 little-endian, 800x480, row by row. sim/render.py --raw makes a PNG of it. */
static esp_err_t screenshot_handler(httpd_req_t *req) {
    if (!ota_authorised(req)) return refuse(req);
    lvgl_port_lock(0);
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    lvgl_port_unlock();
    if (!snap) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/octet-stream");
    esp_err_t err = ESP_OK;
    uint32_t row_bytes = snap->header.w * 2;
    for (uint32_t y = 0; y < snap->header.h && err == ESP_OK; y += SCREENSHOT_ROWS) {
        uint32_t rows = snap->header.h - y < SCREENSHOT_ROWS ? snap->header.h - y : SCREENSHOT_ROWS;
        if (snap->header.stride == row_bytes) {
            err = httpd_resp_send_chunk(req, (const char *)snap->data + (size_t)y * row_bytes, rows * row_bytes);
        } else {
            for (uint32_t r = 0; r < rows && err == ESP_OK; r++) {
                err = httpd_resp_send_chunk(req, (const char *)snap->data + (size_t)(y + r) * snap->header.stride, row_bytes);
            }
        }
    }
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    lvgl_port_lock(0);
    lv_draw_buf_destroy(snap);
    lvgl_port_unlock();
    return err;
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    setenv("TZ", AC_TIMEZONE, 1); /* secrets.ini `timezone`: the hub's clock is shown in it */
    tzset();

    request_queue = xQueueCreate(2, KEY_LEN);
    ESP_ERROR_CHECK(board_init());
    lvgl_port_lock(0);
    ui_init(antonio_start, (size_t)(antonio_end - antonio_start));
    ui_set_link(UI_LINK_WIFI, AC_WIFI_SSID);
    ui_set_focus_handler(on_request);
    ui_set_new_handler(on_new);
    lv_timer_create(backlight_timer, 1000, NULL);
    lvgl_port_unlock();
    vTaskDelay(pdMS_TO_TICKS(200)); /* let the first frame land before lighting the panel */
    board_backlight(true);
    backlight_on = true;

    ESP_ERROR_CHECK(wifi_start(AC_WIFI_SSID, AC_WIFI_PASSWORD, AC_HOSTNAME));
    ESP_ERROR_CHECK(ota_start(AC_DEVICE_KEY, AC_HOSTNAME, OTA_CONFIRM_SECONDS));
    const httpd_uri_t screenshot_uri = {.uri = "/screenshot", .method = HTTP_GET, .handler = screenshot_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(ota_httpd(), &screenshot_uri));
    const httpd_uri_t tap_uri = {.uri = "/tap", .method = HTTP_POST, .handler = tap_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(ota_httpd(), &tap_uri));
    const httpd_uri_t screen_get = {.uri = "/screen", .method = HTTP_GET, .handler = screen_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(ota_httpd(), &screen_get));
    const httpd_uri_t screen_post = {.uri = "/screen", .method = HTTP_POST, .handler = screen_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(ota_httpd(), &screen_post));
#if AC_LEDS_GPIO >= 0
    err = leds_start(ota_httpd()); /* the optional LEDs: never worth stopping the panel for */
    if (err != ESP_OK) ESP_LOGW(TAG, "leds: %s", esp_err_to_name(err));
#endif

    xTaskCreatePinnedToCore(fetch_task, "fetch", 12 * 1024, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(request_task, "request", 8 * 1024, NULL, 3, NULL, 0);
}
