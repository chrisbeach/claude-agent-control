/* The LVGL screen: an LCARS-style frame (double elbow, Antonio Bold, a pastel
 * palette) around a list of Claude Code sessions. Tap a session to bring its
 * window forward on its PC; tap the "+" for a new session; tap a host block in
 * the leg to show only that host. Pure LVGL (no ESP-IDF) so the desktop simulator
 * renders it too. The caller owns LVGL locking. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "model.h"

typedef enum {
    UI_LINK_WIFI,      /* joining Wi-Fi: detail = SSID */
    UI_LINK_FETCHING,  /* Wi-Fi up, nothing fetched yet */
    UI_LINK_OK,        /* showing fresh state */
    UI_LINK_LOST,      /* had state, now cannot reach the hub: the list is withdrawn, never left stale */
    UI_LINK_ERROR,     /* the hub answered with an error: detail = its message */
} ui_link_t;

/* `ttf` is Antonio Bold, the one face on the screen. */
void ui_init(const void *ttf, size_t ttf_len);

/* Takes a copy. The model's `now` becomes the screen's clock, carried forward by LVGL ticks. */
void ui_set_model(const ac_model_t *model);

void ui_set_link(ui_link_t link, const char *detail);

/* True while any session is working, running or needs input, or has finished and
 * has a result waiting to be looked at (done, failed): the backlight follows it. */
bool ui_attention(void);

/* A state's colour on the screen (0xRRGGBB): the pill, the title and the header figure all take it. */
uint32_t ui_status_colour(ac_status_t status);

/* True for a state whose pill pulses (NEEDS INPUT). A pulse eases the opacity from full down to
 * UI_PULSE_LOW and back, UI_PULSE_MS each way; that state's LED (leds.c) copies it. */
#define UI_PULSE_MS 800
#define UI_PULSE_LOW 96 /* of 255 */
bool ui_status_pulses(ac_status_t status);

/* A tap on a session highlights its row and asks for its window to be raised:
 * `handler` gets the session's key, in LVGL's context, so it must only queue the
 * request. The answer comes back through ui_set_focus_result and is printed in
 * that row, in place of its subtitle; a few seconds later the row is one of the
 * list again. */
typedef void (*ui_focus_handler_t)(const char *key);
void ui_set_focus_handler(ui_focus_handler_t handler);
void ui_set_focus_result(const char *key, bool ok, const char *message);

/* A tap on the "+" asks for a new session, the same way: `handler` only queues the
 * request, and ui_set_new_result prints what became of it beside the "+". Until
 * then further taps on the "+" do nothing. */
typedef void (*ui_new_handler_t)(void);
void ui_set_new_handler(ui_new_handler_t handler);
void ui_set_new_result(bool ok, const char *message);

/* What a tap does; public so the simulator and the firmware's /tap can do it. */
void ui_tap_row(int row);              /* row index in the shown list */
void ui_tap_new(void);                 /* the "+" */
void ui_set_filter(const char *host);  /* NULL or "" shows every host */
