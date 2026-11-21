/* Headless renderer: draws the UI once into an RGB565 buffer and writes it raw.
 *   sim <fixture.json | wifi | fetching | lost | error> <none | tap:ROW | raised:ROW | refused:ROW | new | new-opened | new-refused | filter:HOST | scroll:PX> <out.raw>
 * render.py turns the raw frame into PNGs. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fleet.h"
#include "lvgl.h"
#include "ui.h"

#define W 800
#define H 480

static uint16_t frame[W * H];
static uint32_t fake_ms;

static uint32_t tick_cb(void) { return fake_ms; }

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    (void)area; /* full render mode: px_map is the whole frame */
    memcpy(frame, px_map, sizeof(frame));
    lv_display_flush_ready(disp);
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (fread(buf, 1, n, f) != (size_t)n) exit(1);
    buf[n] = '\0';
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static char tapped_key[64];
static void sim_focus(const char *key) { snprintf(tapped_key, sizeof(tapped_key), "%s", key); }

static void pump(int frames) {
    for (int i = 0; i < frames; i++) {
        fake_ms += 40;
        lv_timer_handler();
    }
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <fixture|state> <none|tap:ROW|raised:ROW|refused:ROW|new|new-opened|new-refused|filter:HOST|scroll:PX> <out.raw>\n", argv[0]);
        return 2;
    }
    const char *what = argv[1], *action = argv[2];
    if (!getenv("TZ")) setenv("TZ", "UTC0", 1); /* the firmware takes its zone from secrets.ini; here, TZ=... make png */
    tzset();

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(W, H);
    static uint16_t draw_buf[W * H];
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, flush_cb);

    size_t ttf_len;
    char *ttf = read_file(SIM_TTF, &ttf_len);
    ui_init(ttf, ttf_len);

    if (strcmp(what, "wifi") == 0) {
        ui_set_link(UI_LINK_WIFI, "MYNETWORK");
    } else if (strcmp(what, "fetching") == 0) {
        ui_set_link(UI_LINK_FETCHING, NULL);
    } else {
        bool lost = strcmp(what, "lost") == 0, error = strcmp(what, "error") == 0;
        size_t len;
        char *json = read_file(lost || error ? "fixtures/fleet_mixed.json" : what, &len);
        static ac_model_t model;
        char err[96];
        if (!ac_fleet_parse(json, len, &model, err, sizeof(err))) {
            fprintf(stderr, "parse failed: %s\n", err);
            return 1;
        }
        ui_set_model(&model);
        if (lost) ui_set_link(UI_LINK_LOST, NULL);
        if (error) ui_set_link(UI_LINK_ERROR, "WRONG DEVICE KEY");
    }
    const char *row = strchr(action, ':');
    if (strncmp(action, "tap:", 4) == 0 || strncmp(action, "raised:", 7) == 0 || strncmp(action, "refused:", 8) == 0) {
        ui_set_focus_handler(sim_focus);
        ui_tap_row(atoi(row + 1));
        if (action[0] == 'r') {
            bool ok = strncmp(action, "raised:", 7) == 0;
            char raised[64], host[32];
            snprintf(host, sizeof(host), "%s", tapped_key);
            for (char *c = host; *c; c++) *c = *c == ':' ? '\0' : (char)toupper((unsigned char)*c);
            snprintf(raised, sizeof(raised), "WINDOW RAISED ON %s", host);
            ui_set_focus_result(tapped_key, ok, ok ? raised : "WINDOW SWITCHING IS NOT SET UP ON THE HUB");
        }
    }
    if (strncmp(action, "new", 3) == 0) {
        ui_tap_new();
        if (strcmp(action, "new-opened") == 0) ui_set_new_result(true, "NEW SESSION OPENED ON DEVBOX");
        if (strcmp(action, "new-refused") == 0) ui_set_new_result(false, "NOBODY IS LOGGED IN AT DEVBOX'S DESKTOP");
    }
    if (strncmp(action, "filter:", 7) == 0) ui_set_filter(action + 7);
    pump(5);
    if (strncmp(action, "scroll:", 7) == 0) {
        lv_obj_t *scr = lv_screen_active(); /* the list is the screen's one scrollable child */
        for (uint32_t i = 0; i < lv_obj_get_child_count(scr); i++) {
            lv_obj_t *child = lv_obj_get_child(scr, (int32_t)i);
            if (lv_obj_has_flag(child, LV_OBJ_FLAG_SCROLLABLE)) lv_obj_scroll_to_y(child, atoi(action + 7), LV_ANIM_OFF);
        }
    }
    pump(5); /* settled, and the pulsing pills are still near full brightness */
    if (getenv("SIM_EXTRA_FRAMES")) pump(atoi(getenv("SIM_EXTRA_FRAMES"))); /* to look at an animation part-way */

    FILE *out = fopen(argv[3], "wb");
    if (!out) {
        perror(argv[3]);
        return 1;
    }
    fwrite(frame, sizeof(frame), 1, out);
    fclose(out);
    printf("backlight %s\n", ui_attention() ? "held on" : "free to go dark"); /* what the firmware's backlight timer would be told */
    return 0;
}
