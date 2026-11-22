/* Headless renderer: draws the UI once into an RGB565 buffer and writes it raw.
 *   sim <fixture.json | wifi | fetching | lost | error> <none | tap:ROW | raised:ROW | refused:ROW | new | new-opened | new-refused | filter:HOST | scroll:PX> <out.raw>
 * render.py turns the raw frame into PNGs.
 *
 * Or plays a demo through a simulated touch screen, for the README's animations:
 *   sim <fixture.json> play:STEP;STEP;... <out.raw>
 * writes a frame every FRAME_MS, one after another, and prints a line per touch:
 *   touch <first frame> <last frame> <x> <y>     (the finger is down on those frames)
 * Steps: wait:MS | tap:row:N | tap:host:NAME | tap:plus | raised | refused | opened.
 * A tap presses the screen where that row, host block or "+" is drawn, so LVGL turns it into a
 * click as it would on the board; raised/refused/opened are the hub's answers. */
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

#define FRAME_MS 40
#define TAP_FRAMES 4 /* how long a finger stays down: a click to LVGL, long enough to see */
#define NEW_OPENED "NEW SESSION OPENED ON DEVBOX" /* the hub's answer to the "+" */

static uint16_t frame[W * H];
static uint32_t fake_ms;
static FILE *frames_out; /* play: every frame goes here */
static int n_frames;
static lv_point_t finger;
static bool finger_down;

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
        fake_ms += FRAME_MS;
        lv_timer_handler();
        if (frames_out) {
            fwrite(frame, sizeof(frame), 1, frames_out);
            n_frames++;
        }
    }
}

static void load_model(const char *path) {
    size_t len;
    char *json = read_file(path, &len);
    static ac_model_t model;
    char err[96];
    if (!ac_fleet_parse(json, len, &model, err, sizeof(err))) {
        fprintf(stderr, "parse failed: %s\n", err);
        exit(1);
    }
    free(json);
    ui_set_model(&model);
}

/* The hub's answer to a tap on a row, as the firmware would print it. */
static void focus_result(bool ok) {
    char raised[96], host[sizeof(tapped_key)];
    snprintf(host, sizeof(host), "%s", tapped_key);
    for (char *c = host; *c; c++) *c = *c == ':' ? '\0' : (char)toupper((unsigned char)*c);
    snprintf(raised, sizeof(raised), "WINDOW RAISED ON %s", host);
    ui_set_focus_result(tapped_key, ok, ok ? raised : "WINDOW SWITCHING IS NOT SET UP ON THE HUB");
}

/* the session list: of the screen's scrollable children (labels are too), the one with a box per row */
static lv_obj_t *session_list(void) {
    lv_obj_t *scr = lv_screen_active(), *list = NULL;
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); i++) {
        lv_obj_t *child = lv_obj_get_child(scr, (int32_t)i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_SCROLLABLE) &&
            (!list || lv_obj_get_child_count(child) > lv_obj_get_child_count(list)))
            list = child;
    }
    return list;
}

/* a host's block in the leg: the screen-level box holding a label with its name */
static lv_obj_t *host_block(lv_obj_t *obj, const char *name) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
        if (lv_obj_check_type(child, &lv_label_class) && strcmp(lv_label_get_text(child), name) == 0 &&
            lv_obj_get_parent(obj) == lv_screen_active())
            return obj;
        lv_obj_t *found = host_block(child, name);
        if (found) return found;
    }
    return NULL;
}

/* the "+": the clickable screen-level object furthest into the bottom right corner */
static lv_obj_t *plus_key(void) {
    lv_obj_t *scr = lv_screen_active(), *best = NULL;
    int32_t best_reach = -1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); i++) {
        lv_obj_t *child = lv_obj_get_child(scr, (int32_t)i);
        if (!lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE) || lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_area_t a;
        lv_obj_get_coords(child, &a);
        if (a.x2 + a.y2 > best_reach) best_reach = a.x2 + a.y2, best = child;
    }
    return best;
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    data->point = finger;
    data->state = finger_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void tap(const char *what) {
    lv_obj_t *target = NULL;
    if (strncmp(what, "row:", 4) == 0) {
        lv_obj_t *list = session_list();
        int row = atoi(what + 4);
        if (list && row >= 0 && row < (int)lv_obj_get_child_count(list)) target = lv_obj_get_child(list, row);
    } else if (strncmp(what, "host:", 5) == 0) {
        target = host_block(lv_screen_active(), what + 5);
    } else if (strcmp(what, "plus") == 0) {
        target = plus_key();
    }
    if (!target || lv_obj_has_flag(target, LV_OBJ_FLAG_HIDDEN)) {
        fprintf(stderr, "nothing to tap: %s\n", what);
        exit(1);
    }
    lv_area_t a;
    lv_obj_get_coords(target, &a);
    finger.x = (a.x1 + a.x2) / 2;
    finger.y = (a.y1 + a.y2) / 2;
    finger_down = true;
    int first = n_frames;
    pump(TAP_FRAMES);
    finger_down = false;
    printf("touch %d %d %d %d\n", first, n_frames - 1, (int)finger.x, (int)finger.y);
}

static void play(char *steps) {
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read);
    pump(5); /* settle before the first frame is kept */
    for (char *step = strtok(steps, ";"); step; step = strtok(NULL, ";")) {
        if (strncmp(step, "wait:", 5) == 0) pump(atoi(step + 5) / FRAME_MS);
        else if (strncmp(step, "tap:", 4) == 0) tap(step + 4);
        else if (strcmp(step, "raised") == 0 || strcmp(step, "refused") == 0) focus_result(step[2] == 'i');
        else if (strcmp(step, "opened") == 0) ui_set_new_result(true, NEW_OPENED);
        else {
            fprintf(stderr, "unknown step: %s\n", step);
            exit(1);
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <fixture|state> <none|tap:ROW|raised:ROW|refused:ROW|new|new-opened|new-refused|filter:HOST|scroll:PX|play:STEPS> <out.raw>\n", argv[0]);
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
        load_model(lost || error ? "fixtures/fleet_mixed.json" : what);
        if (lost) ui_set_link(UI_LINK_LOST, NULL);
        if (error) ui_set_link(UI_LINK_ERROR, "WRONG DEVICE KEY");
    }
    if (strncmp(action, "play:", 5) == 0) {
        frames_out = fopen(argv[3], "wb");
        if (!frames_out) {
            perror(argv[3]);
            return 1;
        }
        ui_set_focus_handler(sim_focus);
        play(strdup(action + 5));
        fclose(frames_out);
        return 0;
    }
    const char *row = strchr(action, ':');
    if (strncmp(action, "tap:", 4) == 0 || strncmp(action, "raised:", 7) == 0 || strncmp(action, "refused:", 8) == 0) {
        ui_set_focus_handler(sim_focus);
        ui_tap_row(atoi(row + 1));
        if (action[0] == 'r') focus_result(strncmp(action, "raised:", 7) == 0);
    }
    if (strncmp(action, "new", 3) == 0) {
        ui_tap_new();
        if (strcmp(action, "new-opened") == 0) ui_set_new_result(true, NEW_OPENED);
        if (strcmp(action, "new-refused") == 0) ui_set_new_result(false, "NOBODY IS LOGGED IN AT DEVBOX'S DESKTOP");
    }
    if (strncmp(action, "filter:", 7) == 0) ui_set_filter(action + 7);
    pump(5);
    if (strncmp(action, "scroll:", 7) == 0) lv_obj_scroll_to_y(session_list(), atoi(action + 7), LV_ANIM_OFF);
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
