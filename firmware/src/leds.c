/* Optional status lights: a WS2812B strip on AC_LEDS_GPIO (secrets.ini `leds_gpio`; GPIO 6 is "AD"
 * on the Sensor header, the board's only free pin), powered from that header's 3V3. Only the first
 * LEDS light, one per band (a band = one of the header's three counts); the rest of an uncut strip
 * is sent black so none stays lit from power-up. LED 0 is the left band, unless AC_LEDS_REVERSED
 * (secrets.ini `leds_reversed`) says the strip was fitted from the other end.
 *
 * A band whose state's pill pulses (NEEDS INPUT) pulses the same way: the pill's period, depth and
 * ease (ui.h), redrawn every FRAME_MS by a task that sleeps while nothing pulses. The pulse starts
 * at full brightness when the band starts pulsing, as the pill does on the same fleet answer.
 *
 * POST /leds?c=RRGGBB            all bands one colour
 * POST /leds?c=RRGGBB,RRGGBB,... one colour per band, left band first
 * POST /leds?c=...&p=010         and these bands pulse (one digit per band, left band first)
 * (bearer = the device key): a test until the next fleet answer, when leds_set() takes over.
 *
 * leds_dark(true) (POST /screen?on=0, main.c) blacks every band out until leds_dark(false);
 * colours set meanwhile are kept and shown then.
 * Every colour is scaled by LEDS_CAP/255, hue kept: the strip runs off the board's 3.3 V
 * regulator, which also feeds the ESP32. */
#include "leds.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "lvgl.h" /* lv_cubic_bezier: the pill's own ease */
#include "ota.h"
#include "ui.h"

#ifndef AC_LEDS_GPIO
#define AC_LEDS_GPIO 6
#endif
#ifndef AC_LEDS_REVERSED
#define AC_LEDS_REVERSED 0
#endif
#define LEDS 3
#define LEDS_CLEARED 60 /* a whole metre of 60/m, in case the strip is not cut yet */
#define LEDS_CAP 96     /* of 255 per channel: 3 LEDs at most ~70 mA */
#define FRAME_MS 20     /* a pulsing band is redrawn 50 times a second */

static const char *TAG = "leds";
static led_strip_handle_t strip;
static SemaphoreHandle_t lock; /* rgb, pulsing and the strip: the fetch task, the HTTP server and the pulse task */
static TaskHandle_t pulser;
static uint8_t rgb[LEDS][3] = {{160, 110, 50}, {160, 110, 50}, {160, 110, 50}}; /* per band, left first; warm white at boot, already scaled */
static uint8_t pulsing;       /* bit b: band b pulses */
static int64_t pulse_from_us; /* when the bands started pulsing: the pulse's phase */
static esp_err_t last_err = ESP_OK;
static bool dark;             /* leds_dark(): every band black, its colour kept for when the screen is back on */

static uint8_t scaled(uint32_t channel) { return (uint8_t)(channel * LEDS_CAP / 255); }

/* How much of a pulsing band's light shows `now_us` into its pulse, 0..1. The pill's opacity goes
 * 255 → UI_PULSE_LOW → 255 on the pill's ease-in-out. The screen blends that opacity in sRGB, while
 * an LED's light is linear, so the band takes opacity^2.2 to dip as deep to the eye (96/255 → 12 %). */
static float pulse_light(int64_t now_us) {
    int32_t ms = (int32_t)((now_us - pulse_from_us) / 1000 % (2 * UI_PULSE_MS));
    int32_t t = (ms < UI_PULSE_MS ? ms : 2 * UI_PULSE_MS - ms) * LV_BEZIER_VAL_MAX / UI_PULSE_MS;
    int32_t eased = lv_cubic_bezier(t, LV_BEZIER_VAL_FLOAT(0.42), 0, LV_BEZIER_VAL_FLOAT(0.58), LV_BEZIER_VAL_MAX);
    float opa = (255.0f - (255 - UI_PULSE_LOW) * (float)eased / LV_BEZIER_VAL_MAX) / 255.0f;
    return powf(opa, 2.2f);
}

/* Sends the bands to the strip, pulsing ones at `light`. Lock held. */
static void show_locked(float light) {
    if (!strip) return;
    for (int i = 0; i < LEDS_CLEARED; i++) {
        if (i < LEDS) {
            int band = AC_LEDS_REVERSED ? LEDS - 1 - i : i;
            const uint8_t *c = rgb[band];
            float k = dark ? 0.0f : pulsing & (1 << band) ? light : 1.0f;
            led_strip_set_pixel(strip, i, (uint32_t)(c[0] * k + 0.5f), (uint32_t)(c[1] * k + 0.5f),
                                (uint32_t)(c[2] * k + 0.5f));
        } else {
            led_strip_set_pixel(strip, i, 0, 0, 0);
        }
    }
    esp_err_t err = led_strip_refresh(strip);
    if (err != ESP_OK && err != last_err) ESP_LOGW(TAG, "refresh: %s", esp_err_to_name(err)); /* once, not 50/s */
    last_err = err;
}

/* New colours (already scaled) and pulses for the bands, from the fleet or a test. */
static void apply(const uint8_t next[LEDS][3], uint8_t pulse) {
    if (!lock) return; /* no LEDs configured, or not started yet */
    xSemaphoreTake(lock, portMAX_DELAY);
    if (memcmp(next, rgb, sizeof(rgb)) == 0 && pulse == pulsing) { /* most long-poll answers change nothing */
        xSemaphoreGive(lock);
        return;
    }
    int64_t now = esp_timer_get_time();
    if (pulse && !pulsing) pulse_from_us = now; /* from full brightness, as the pill */
    memcpy(rgb, next, sizeof(rgb));
    pulsing = pulse;
    show_locked(pulse ? pulse_light(now) : 1.0f);
    xSemaphoreGive(lock);
    if (pulse && pulser) xTaskNotifyGive(pulser);
}

static void pulse_task(void *arg) {
    for (;;) {
        xSemaphoreTake(lock, portMAX_DELAY);
        bool on = pulsing != 0 && !dark;
        if (on) show_locked(pulse_light(esp_timer_get_time()));
        xSemaphoreGive(lock);
        if (on) vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
        else ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* until apply() starts a pulse */
    }
}

static bool parse_colour(const char *s, uint8_t out[3]) {
    if (strlen(s) < 6) return false;
    for (int i = 0; i < 6; i++) {
        if (!strchr("0123456789abcdefABCDEF", s[i])) return false;
    }
    for (int k = 0; k < 3; k++) {
        char two[3] = {s[2 * k], s[2 * k + 1], 0};
        long v = strtol(two, NULL, 16);
        out[k] = scaled((uint32_t)v);
    }
    return true;
}

static esp_err_t leds_handler(httpd_req_t *req) {
    if (!ota_authorised(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "bad or missing bearer token\n");
    }
    char query[96], value[80], flags[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "c", value, sizeof(value)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "c=RRGGBB or c=RRGGBB,RRGGBB,RRGGBB is required, p=010 optional\n");
    }
    uint8_t next[LEDS][3];
    const char *p = value;
    int n = 0;
    while (n < LEDS && *p) {
        if (!parse_colour(p, next[n])) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "colours are six hex digits, comma separated\n");
        }
        n++;
        p += 6;
        if (*p == ',') p++;
    }
    for (int i = n; i < LEDS; i++) memcpy(next[i], next[n - 1], 3); /* one colour = all bands */
    uint8_t pulse = 0;
    if (httpd_query_key_value(query, "p", flags, sizeof(flags)) == ESP_OK) {
        for (int b = 0; b < LEDS && flags[b]; b++) {
            if (flags[b] == '1') pulse |= 1 << b;
        }
    }
    apply(next, pulse);
    char said[64];
    snprintf(said, sizeof(said), "%02x%02x%02x,%02x%02x%02x,%02x%02x%02x p=%d%d%d\n", next[0][0], next[0][1],
             next[0][2], next[1][0], next[1][1], next[1][2], next[2][0], next[2][1], next[2][2], pulse & 1,
             (pulse >> 1) & 1, (pulse >> 2) & 1);
    return httpd_resp_sendstr(req, said);
}

void leds_dark(bool on) {
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    dark = on;
    show_locked(pulsing ? pulse_light(esp_timer_get_time()) : 1.0f);
    xSemaphoreGive(lock);
    if (!on && pulser) xTaskNotifyGive(pulser); /* a pulse that was held dark goes on */
}

void leds_set(const uint32_t colours[3], uint8_t pulse) {
    uint8_t next[LEDS][3];
    for (int i = 0; i < LEDS; i++) {
        /* Fully saturated, same hue: the screen's pastels read as near white through a diffuser. */
        uint32_t c[3] = {(colours[i] >> 16) & 0xFF, (colours[i] >> 8) & 0xFF, colours[i] & 0xFF};
        uint32_t lo = c[0], hi = c[0];
        for (int k = 1; k < 3; k++) {
            if (c[k] < lo) lo = c[k];
            if (c[k] > hi) hi = c[k];
        }
        for (int k = 0; k < 3; k++) next[i][k] = scaled(hi > lo ? (c[k] - lo) * 255 / (hi - lo) : c[k]);
    }
    apply(next, pulse);
}

esp_err_t leds_start(httpd_handle_t server) {
    lock = xSemaphoreCreateMutex();
    led_strip_config_t config = {
        .strip_gpio_num = AC_LEDS_GPIO,
        .max_leds = LEDS_CLEARED,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = true, /* the RGB panel keeps the CPUs and PSRAM busy: no refills by interrupt */
    };
    esp_err_t err = led_strip_new_rmt_device(&config, &rmt, &strip);
    if (err != ESP_OK) return err;
    xSemaphoreTake(lock, portMAX_DELAY);
    show_locked(1.0f);
    xSemaphoreGive(lock);
    if (xTaskCreatePinnedToCore(pulse_task, "leds", 4096, NULL, 2, &pulser, 0) != pdPASS) return ESP_ERR_NO_MEM;
    const httpd_uri_t uri = {.uri = "/leds", .method = HTTP_POST, .handler = leds_handler};
    return httpd_register_uri_handler(server, &uri);
}
