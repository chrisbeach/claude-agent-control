#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

/* ---- geometry (800x480) --------------------------------------------------
 * Two frames whose elbows converge on the left, the classic LCARS double elbow. The upper
 * frame's leg comes down from the top edge and turns right into a bar under the header (clock,
 * the three counts, the screen's name); a gap; the lower frame's bar turns down into the leg of
 * host blocks beside the session list. Outer corners round at about half the leg's width,
 * inner corners tight, bars kept thin to leave room for the list. */
#define SW 800
#define SH 480
#define BAR 10 /* the elbows' horizontal bars; must stay >= R_IN so the bite's far corners clip */
#define LEG 96
#define GAP 4
#define R_OUT 46 /* about half the leg, as before */
#define R_IN 10
#define TURN_W (LEG + 60) /* the elbow runs 60 past the leg before its bar's segment */
#define CLEAR 20 /* between the frame and what it holds: the header over the upper bar, the list under the lower */
#define HEAD_TOP 6                               /* the header's figures start this far down */
#define UPPER_Y (HEAD_TOP + 49 + CLEAR)          /* the upper frame's bar: under the header (figure, gap, word) */
#define LOWER_Y (UPPER_Y + BAR + GAP)            /* the lower frame's bar */
#define TURN_H R_OUT                             /* the lower elbow, bar and turn, above the first leg block */
#define LEG_Y (LOWER_Y + TURN_H + GAP)
#define LEG_H (SH - LEG_Y)
#define VIEW_X (LEG + 24)
#define VIEW_Y (LOWER_Y + BAR + CLEAR - ROW_TOP) /* a row's pill sits ROW_TOP down in it */
#define VIEW_W (SW - VIEW_X)
#define VIEW_H (SH - VIEW_Y)
#define ROW_H 64
#define ROW_TOP 8 /* the row's 48 px of contents sit centred in it: 16 px of black between rows, no rule */
#define PILL_W 128
#define PILL_H 28
#define TEXT_X (PILL_W + 14)
#define TEXT_W (VIEW_W - TEXT_X - 14)
#define TODO_PENDING "SUMMARISING..." /* the hub's to-do line while it is being written (fleet.TODO_PENDING) */
#define HOST_W 84 /* the host, right-aligned at the end of the title's line */
#define SEG_W 44  /* a bar's pip, just past its elbow */
#define ROW_PAD 8 /* the tapped row's band reaches this far left of the row's contents ... */
#define ROW_END 6 /* ... and stops this short of the screen's edge, where its round end shows */
#define BAND_R 10
#define PLUS_D 60
#define PLUS_MARGIN 12
#define PLUS_BAR_L 26
#define PLUS_BAR_T 6
/* The header's three counts (WORKING, NEEDS INPUT, DONE, left to right) sit on the screen's centre line and 86 px
 * (16.5 mm of glass) either side of it, so that three LEDs mounted above the screen at that spacing (leds.c) each light
 * over their own count. */
#define LIGHT_PITCH 86
#define LIGHT_X(i) (SW / 2 + ((i) - 1) * LIGHT_PITCH)
#define COUNT_GAP 5 /* between a count's figure and its word */
#define NOTE_PAD_X 10
#define NOTE_PAD_Y 6
#define LEG_BLOCKS AC_MAX_HOSTS
#define GAUGE_D 32     /* an allowance window's ring gauge, right of the counts: diameter ... */
#define GAUGE_W 4      /* ... ring width ... */
#define GAUGE_PITCH 46 /* ... and least spacing (more when a label is wider); those shown are centred between DONE and AGENTS */
#define GAUGE_GAP 12   /* the least room between two gauges' labels */
#define LEG_PAD 5
#define LEG_LINE 4 /* between a host's count and its name */

/* ---- palette ---------------------------------------------------------------- */
#define C_BLACK 0x000000
#define C_TAN 0xE8A85F
#define C_ROSE 0xAF7265 /* the elbows, clock and AGENTS: a dusty rose sampled from a classic LCARS panel */
#define C_SALMON 0xE07A6A
#define C_LAV 0xB994C9
#define C_PALE 0xF6E3C5
#define C_GREY 0x55556A
#define C_GREYPALE 0x8A8AA0
#define C_BLUEPALE 0xB9D4FF
#define C_GREEN 0x73BF69
#define C_AMBER 0xFFAA33
#define C_RED 0xFF5544

#define SUB_BRIGHTNESS 72 /* a subtitle is its row's colour a shade darker */
#define USAGE_AMBER 75
#define USAGE_RED 90
#define QUIET_BRIGHTNESS 60 /* a row with nothing for the person to do: finished with nothing asked, or still working */
#define BAND_BRIGHTNESS 30 /* the tapped row's band is its colour, well dimmed */
#define GLINT_W 56        /* a WORKING pill has a soft light passing across it, left to right ... */
#define GLINT_OPA 64      /* ... this bright at its middle, fading to nothing either side */
#define GLINT_MS 1400     /* ... taking this long to cross */
#define GLINT_PAUSE_MS 900 /* ... then gone this long before the next */
#define REQUEST_MS 25000 /* a tap's request shows as under way this long at most: past the firmware's own HTTP timeout */
#define DONE_MS 4000     /* how long the answer stays up when it worked ... */
#define REFUSED_MS 10000 /* ... and when it did not: there is a reason to read */

/* One colour and one word per state: the pill, the title and the figures all take it. */
static const struct {
    const char *word;
    uint32_t colour;
    bool lit; /* a filled pill; unlit states get an outline */
} STATUS[AC_STATUS_COUNT] = {
    [AC_WORKING] = {"WORKING", C_BLUEPALE, true}, [AC_WAITING] = {"NEEDS INPUT", C_AMBER, true},
    [AC_IDLE] = {"IDLE", C_LAV, false},           [AC_RUNNING] = {"RUNNING", C_BLUEPALE, true},
    [AC_DONE] = {"DONE", C_GREEN, true},          [AC_FAILED] = {"FAILED", C_RED, true},
    [AC_STOPPED] = {"STOPPED", C_GREYPALE, false},
};

static lv_font_t *F15, *F18, *F20, *F26, *F34, *F44; /* F15 for the leg's host names only */

typedef struct {
    lv_obj_t *row, *pill, *pill_text, *glint, *dur, *tok, *title, *host, *sub;
    int session; /* index into W.model.sessions, -1 when unused */
    bool pulsing, glinting;
} row_t;

/* What became of a tap's request, printed for a while beside what was tapped. */
typedef struct {
    char text[100];
    uint32_t colour;
    bool pending;      /* asked, not yet answered */
    uint32_t tick, ms; /* up until `ms` after `tick`; ms 0 = nothing is up */
} note_t;

static struct {
    ac_model_t model;
    bool have_model;
    uint32_t model_tick; /* lv_tick_get() when the model arrived */
    ui_link_t link;
    char filter[sizeof(((ac_host_t *)0)->name)];
    int last_minute;
    ui_focus_handler_t focus_handler;
    ui_new_handler_t new_handler;
    char tapped[sizeof(((ac_session_t *)0)->key)]; /* the session last tapped: its row is highlighted while tap_note is up */
    note_t tap_note;                               /* printed in that row, in place of its subtitle */
    note_t new_note;                               /* the "+"'s, printed beside it */

    lv_obj_t *pip[2], *run[2]; /* each bar's pip and the frame after it: upper, lower */
    lv_obj_t *slab[2];         /* each elbow's body: upper, lower */
    lv_obj_t *clock, *date;
    lv_obj_t *count_fig[3], *count_lbl[3];
    lv_obj_t *hero;
    int gauge_mid; /* x halfway between the DONE word's right end and AGENTS' left */
    struct {
        lv_obj_t *block, *fig, *name;
    } leg[LEG_BLOCKS];
    struct {
        lv_obj_t *ring, *fig, *name;
    } gauge[AC_MAX_USAGE];
    lv_obj_t *list;
    row_t rows[AC_MAX_SESSIONS];
    int n_rows;
    lv_obj_t *message, *message_title, *message_detail;
    lv_obj_t *plus, *plus_note;
} W;

/* ---- small helpers -------------------------------------------------------- */

static lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

static lv_color_t shade(uint32_t hex, int percent) {
    return lv_color_make((uint8_t)(((hex >> 16) & 0xFF) * percent / 100), (uint8_t)(((hex >> 8) & 0xFF) * percent / 100),
                         (uint8_t)((hex & 0xFF) * percent / 100));
}

/* Antonio Bold's metrics: capitals are 0.859 em tall and start 0.296 em below the
 * top of the line box. Everything on the screen is capitals and figures, so text
 * is placed by where that ink falls, not by the line box. */
static int cap_h(int font_px) { return (font_px * 859 + 500) / 1000; }
static int cap_top(int font_px) { return (font_px * 296 + 500) / 1000; }
/* y for a label so its capitals sit centred in a band `height` tall starting at `top` */
static int cap_centre(int font_px, int top, int height) { return top + (height - cap_h(font_px)) / 2 - cap_top(font_px); }
/* y for a label so its capitals start at `y` */
static int cap_at(int font_px, int y) { return y - cap_top(font_px); }

static int font_px(const lv_font_t *font) { return font == F15 ? 15 : font == F18 ? 18 : font == F20 ? 20 : font == F26 ? 26 : font == F34 ? 34 : 44; }

static lv_obj_t *box(lv_obj_t *parent) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return b;
}

static lv_obj_t *fill(lv_obj_t *parent, uint32_t colour, int x, int y, int w, int h) {
    lv_obj_t *b = box(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, C(colour), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    return b;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t colour, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, C(colour), 0);
    lv_label_set_text(l, text);
    return l;
}

/* A one-line label of fixed width that ends in dots when the text is longer. */
static lv_obj_t *line(lv_obj_t *parent, const lv_font_t *font, uint32_t colour, int x, int y, int w) {
    lv_obj_t *l = label(parent, font, colour, "");
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, lv_font_get_line_height(font));
    return l;
}

/* lv_label_set_text redraws even when nothing changed; most ticks change nothing. */
static void set_text(lv_obj_t *l, const char *text) {
    if (strcmp(lv_label_get_text(l), text) != 0) lv_label_set_text(l, text);
}

static void set_hidden(lv_obj_t *obj, bool hidden) {
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static int64_t now_s(void) { return W.have_model ? W.model.now + lv_tick_elaps(W.model_tick) / 1000 : 0; }

/* Compact, to share the line under the pill with the session's cost: "0M", "34M", "2H4M", "1D4H", "3D". */
static void format_elapsed(int64_t seconds, char *out, size_t n) {
    if (seconds < 0) seconds = 0;
    if (seconds < 3600) snprintf(out, n, "%dM", (int)(seconds / 60));
    else if (seconds < 86400 && seconds % 3600 < 60) snprintf(out, n, "%dH", (int)(seconds / 3600));
    else if (seconds < 86400) snprintf(out, n, "%dH%dM", (int)(seconds / 3600), (int)(seconds % 3600 / 60));
    else if (seconds < 2 * 86400) snprintf(out, n, "1D%dH", (int)(seconds % 86400 / 3600));
    else snprintf(out, n, "%dD", (int)(seconds / 86400));
}

static void format_clock(int64_t t, const char *fmt, char *out, size_t n) {
    time_t tt = (time_t)t;
    struct tm lt;
    localtime_r(&tt, &lt);
    strftime(out, n, fmt, &lt);
    for (char *p = out; *p; p++) *p = (char)toupper((unsigned char)*p);
}

static bool shown(const ac_session_t *s) { return !W.filter[0] || strcmp(s->host, W.filter) == 0; }

static void note_set(note_t *note, bool pending, uint32_t colour, uint32_t ms, const char *text) {
    snprintf(note->text, sizeof(note->text), "%s", text);
    note->colour = colour;
    note->pending = pending;
    note->tick = lv_tick_get();
    note->ms = ms;
}

static bool note_over(const note_t *note) { return note->ms && lv_tick_elaps(note->tick) > note->ms; }

static void note_clear(note_t *note) { memset(note, 0, sizeof(*note)); }

/* ---- pills ---------------------------------------------------------------- */

static void anim_bg_opa(void *obj, int32_t v) { lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0); }
static void anim_text_opa(void *obj, int32_t v) { lv_obj_set_style_text_opa(obj, (lv_opa_t)v, 0); }

/* `exec` sets what pulses: anim_bg_opa for a fill, anim_text_opa for a label */
static void pulse(lv_obj_t *obj, lv_anim_exec_xcb_t exec, bool on) {
    lv_anim_delete(obj, exec);
    if (!on) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, LV_OPA_COVER, UI_PULSE_LOW);
    lv_anim_set_duration(&a, UI_PULSE_MS);
    lv_anim_set_reverse_duration(&a, UI_PULSE_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void set_pulse(lv_obj_t *obj, bool on) { pulse(obj, anim_bg_opa, on); }

static void anim_x(void *obj, int32_t v) { lv_obj_set_x(obj, v); }

/* The WORKING pill's sign of life: a light that passes across it now and then, under its word. */
static void set_glint(lv_obj_t *glint, bool on) {
    lv_anim_delete(glint, anim_x);
    set_hidden(glint, !on);
    if (!on) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, glint);
    lv_anim_set_exec_cb(&a, anim_x);
    lv_anim_set_values(&a, -GLINT_W, PILL_W);
    lv_anim_set_duration(&a, GLINT_MS);
    lv_anim_set_repeat_delay(&a, GLINT_PAUSE_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static lv_obj_t *pill(lv_obj_t *parent, int x, int y, lv_obj_t **text, lv_obj_t **glint) {
    lv_obj_t *p = box(parent);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, PILL_W, PILL_H);
    lv_obj_set_style_radius(p, PILL_H / 2, 0);
    lv_obj_set_style_clip_corner(p, true, 0); /* the glint stays inside the round ends */
    *glint = box(p);
    lv_obj_set_size(*glint, GLINT_W, PILL_H);
    for (int half = 0; half < 2; half++) { /* fading up to its middle, then down again: no edge to it */
        lv_obj_t *h = box(*glint);
        lv_obj_set_pos(h, half * GLINT_W / 2, 0);
        lv_obj_set_size(h, GLINT_W / 2, PILL_H);
        lv_obj_set_style_bg_color(h, C(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_color(h, C(0xFFFFFF), 0);
        lv_obj_set_style_bg_grad_dir(h, LV_GRAD_DIR_HOR, 0);
        lv_obj_set_style_bg_main_opa(h, half ? GLINT_OPA : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_grad_opa(h, half ? LV_OPA_TRANSP : GLINT_OPA, 0);
    }
    lv_obj_add_flag(*glint, LV_OBJ_FLAG_HIDDEN);
    *text = label(p, F20, C_BLACK, "");
    lv_obj_set_width(*text, PILL_W);
    lv_obj_set_style_text_align(*text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(*text, 0, cap_centre(20, 0, PILL_H));
    return p;
}

static void style_pill(lv_obj_t *p, lv_obj_t *text, ac_status_t status, int brightness) {
    lv_color_t colour = shade(STATUS[status].colour, brightness);
    bool lit = STATUS[status].lit;
    lv_obj_set_style_bg_color(p, colour, 0);
    lv_obj_set_style_bg_opa(p, lit ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(p, colour, 0);
    lv_obj_set_style_border_width(p, lit ? 0 : 2, 0);
    lv_obj_set_style_text_color(text, lit ? C(C_BLACK) : colour, 0);
    set_text(text, STATUS[status].word);
}

/* ---- the frame ------------------------------------------------------------ */

static void host_clicked(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= W.model.n_hosts) return;
    const char *host = W.model.hosts[i].name;
    ui_set_filter(strcmp(W.filter, host) == 0 ? NULL : host);
}

/* An elbow, cut off at TURN_W where its bar's segments take over: a round-cornered slab with a
 * round-cornered black bite out of it, both clipped by the elbow's own box so that one corner of
 * each shows. `down`: the bar is at the top and the leg hangs from it (the lower frame); otherwise
 * the leg comes down from the top edge into a bar at the bottom (the upper frame). */
static void elbow(lv_obj_t *scr, int y, int h, bool down) {
    lv_obj_t *turn = box(scr);
    lv_obj_set_pos(turn, 0, y);
    lv_obj_set_size(turn, TURN_W, h);
    int big = h + R_OUT * 2;
    lv_obj_t *slab = fill(turn, C_ROSE, 0, down ? 0 : h - big, TURN_W + R_OUT * 2, big);
    lv_obj_set_style_radius(slab, R_OUT, 0);
    W.slab[down] = slab;
    lv_obj_t *bite = fill(turn, C_BLACK, LEG, down ? BAR : -BAR, TURN_W, h);
    lv_obj_set_style_radius(bite, R_IN, 0);
}

/* A bar's run right of its elbow: a pip (a small segment set apart by black), only while it says
 * something, then the frame on to the right edge. Without the pip the frame runs on unbroken from
 * the elbow. Both bars have their pips in the same place. */
static void bar_run(lv_obj_t *scr, int y, int i) {
    W.pip[i] = fill(scr, C_TAN, TURN_W + GAP, y, SEG_W, BAR);
    W.run[i] = fill(scr, C_ROSE, TURN_W, y, SW - TURN_W, BAR);
    lv_obj_add_flag(W.pip[i], LV_OBJ_FLAG_HIDDEN);
}

static void set_pip(int i, bool on, uint32_t colour) {
    set_hidden(W.pip[i], !on);
    if (on) lv_obj_set_style_bg_color(W.pip[i], C(colour), 0);
    int x = on ? TURN_W + GAP + SEG_W + GAP : TURN_W;
    lv_obj_set_x(W.run[i], x);
    lv_obj_set_width(W.run[i], SW - x);
}

static void build_frame(lv_obj_t *scr) {
    elbow(scr, 0, UPPER_Y + BAR, false);
    bar_run(scr, UPPER_Y, 0);
    elbow(scr, LOWER_Y, TURN_H, true);
    bar_run(scr, LOWER_Y, 1);

    /* the header, over the upper bar: the clock over the date at the list's left edge; the three
     * counts, each under its light band (figure over word); a ring gauge per allowance window right
     * of them (ring over word); the screen's name hard right */
    int word_top = HEAD_TOP + cap_h(34) + COUNT_GAP;
    W.clock = label(scr, F34, C_ROSE, "");
    lv_obj_set_pos(W.clock, VIEW_X, cap_at(34, HEAD_TOP));
    W.date = label(scr, F18, C_ROSE, "");
    lv_obj_set_pos(W.date, VIEW_X, cap_at(18, word_top));
    static const ac_status_t COUNT_STATUS[3] = {AC_WORKING, AC_WAITING, AC_DONE};
    static const char *const COUNT_WORD[3] = {"WORKING", "NEED INPUT", "DONE"};
    for (int i = 0; i < 3; i++) {
        int left = LIGHT_X(i) - LIGHT_PITCH / 2;
        W.count_fig[i] = label(scr, F34, STATUS[COUNT_STATUS[i]].colour, "");
        W.count_lbl[i] = label(scr, F18, STATUS[COUNT_STATUS[i]].colour, COUNT_WORD[i]);
        lv_obj_set_pos(W.count_fig[i], left, cap_at(34, HEAD_TOP));
        lv_obj_set_pos(W.count_lbl[i], left, cap_at(18, word_top));
        lv_obj_set_width(W.count_fig[i], LIGHT_PITCH);
        lv_obj_set_width(W.count_lbl[i], LIGHT_PITCH);
        lv_obj_set_style_text_align(W.count_fig[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(W.count_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    int ring_y = HEAD_TOP + cap_h(34) / 2 - GAUGE_D / 2; /* centred on the counts' figures */
    for (int i = 0; i < AC_MAX_USAGE; i++) {
        lv_obj_t *ring = lv_arc_create(scr);
        lv_obj_remove_style_all(ring);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_y(ring, ring_y);
        lv_obj_set_size(ring, GAUGE_D, GAUGE_D);
        lv_arc_set_rotation(ring, 270); /* from twelve o'clock, clockwise */
        lv_arc_set_bg_angles(ring, 0, 360);
        lv_arc_set_range(ring, 0, 100);
        for (int part = 0; part < 2; part++) {
            lv_style_selector_t sel = part ? LV_PART_INDICATOR : LV_PART_MAIN;
            lv_obj_set_style_arc_width(ring, GAUGE_W, sel);
            lv_obj_set_style_arc_opa(ring, LV_OPA_COVER, sel);
        }
        lv_obj_set_style_arc_rounded(ring, true, LV_PART_INDICATOR);
        W.gauge[i].ring = ring;
        W.gauge[i].fig = label(scr, F18, C_LAV, "");
        lv_obj_set_width(W.gauge[i].fig, GAUGE_PITCH);
        lv_obj_set_style_text_align(W.gauge[i].fig, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_y(W.gauge[i].fig, cap_centre(18, ring_y, GAUGE_D));
        W.gauge[i].name = label(scr, F18, C_LAV, "");
        lv_obj_set_width(W.gauge[i].name, GAUGE_PITCH);
        lv_obj_set_style_text_align(W.gauge[i].name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_y(W.gauge[i].name, cap_at(18, word_top));
    }
    W.hero = label(scr, F44, C_ROSE, "AGENTS");
    /* in the screen's top right corner, flush: the S's ink ends ~1.5 px inside its advance (measured in the sim) */
    int bearing = 1;
    lv_obj_align(W.hero, LV_ALIGN_TOP_RIGHT, bearing, cap_at(44, 2)); /* Antonio's caps stand 2 px above cap_at's line at 44 px:
                                                                      * 2 down puts their ink on the top row, none cut off */
    int done_right = LIGHT_X(2) + lv_text_get_width(COUNT_WORD[2], strlen(COUNT_WORD[2]), F18, 0) / 2;
    int agents_left = SW + bearing - lv_text_get_width("AGENTS", 6, F44, 0);
    W.gauge_mid = (done_right + agents_left) / 2;

    /* the leg: a block per host */
    for (int i = 0; i < LEG_BLOCKS; i++) {
        lv_obj_t *b = fill(scr, C_TAN, 0, LEG_Y, LEG, LEG_H);
        W.leg[i].block = b;
        W.leg[i].fig = label(b, F26, C_BLACK, "");
        lv_obj_set_width(W.leg[i].fig, LEG - 8);
        lv_obj_set_style_text_align(W.leg[i].fig, LV_TEXT_ALIGN_RIGHT, 0);
        W.leg[i].name = label(b, F15, C_BLACK, "");
        lv_obj_set_width(W.leg[i].name, LEG - 8);
        lv_obj_set_style_text_align(W.leg[i].name, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, host_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (i > 0) lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN); /* until a model arrives the leg is one plain block */
    }
}

static void update_frame(void) {
    bool live = W.have_model && W.link == UI_LINK_OK;
    char text[24];

    for (int i = 0; i < 3; i++) {
        set_hidden(W.count_fig[i], !live);
        set_hidden(W.count_lbl[i], !live);
    }
    if (live) {
        const int counts[3] = {W.model.working, W.model.waiting, W.model.done};
        for (int i = 0; i < 3; i++) {
            snprintf(text, sizeof(text), "%d", counts[i]);
            set_text(W.count_fig[i], text);
        }
    }
    /* the pips: the upper one green while a DONE row wants looking at (not dimmed), the lower one
     * WORKING's blue while anything is working; otherwise the bar runs on without one */
    set_pip(0, live && ac_done_to_check(&W.model) > 0, STATUS[AC_DONE].colour);
    set_pip(1, live && W.model.working > 0, STATUS[AC_WORKING].colour);

    /* the elbows, bars and all: rose, or NEEDS INPUT's amber while a session waits on the person */
    lv_color_t frame = C(live && W.model.waiting > 0 ? STATUS[AC_WAITING].colour : C_ROSE);
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_color(W.slab[i], frame, 0);
        lv_obj_set_style_bg_color(W.run[i], frame, 0);
    }

    /* the allowance gauges: the share used, round from twelve o'clock; amber, then red, as it runs out */
    int n_usage = live ? W.model.n_usage : 0;
    int pitch = GAUGE_PITCH; /* wide enough that the widest word ("FABLE") keeps clear of its neighbours */
    for (int i = 0; i < n_usage; i++) {
        const char *word = W.model.usage[i].label;
        int w = lv_text_get_width(word, strlen(word), F18, 0) + GAUGE_GAP;
        if (w > pitch) pitch = w;
    }
    for (int i = 0; i < AC_MAX_USAGE; i++) {
        bool used = i < n_usage;
        set_hidden(W.gauge[i].ring, !used);
        set_hidden(W.gauge[i].fig, !used);
        set_hidden(W.gauge[i].name, !used);
        if (!used) continue;
        int cx = W.gauge_mid - (n_usage - 1) * pitch / 2 + i * pitch; /* the row of them centred */
        lv_obj_set_x(W.gauge[i].ring, cx - GAUGE_D / 2);
        lv_obj_set_width(W.gauge[i].fig, pitch);
        lv_obj_set_x(W.gauge[i].fig, cx - pitch / 2);
        lv_obj_set_width(W.gauge[i].name, pitch);
        lv_obj_set_x(W.gauge[i].name, cx - pitch / 2);
        const ac_usage_t *u = &W.model.usage[i];
        uint32_t colour = u->percent >= USAGE_RED ? C_RED : u->percent >= USAGE_AMBER ? C_AMBER : C_LAV;
        lv_arc_set_value(W.gauge[i].ring, u->percent < 0 ? 0 : u->percent > 100 ? 100 : u->percent);
        lv_obj_set_style_arc_color(W.gauge[i].ring, shade(colour, 30), LV_PART_MAIN);
        lv_obj_set_style_arc_color(W.gauge[i].ring, C(colour), LV_PART_INDICATOR);
        lv_obj_set_style_text_color(W.gauge[i].fig, C(colour), 0);
        lv_obj_set_style_text_color(W.gauge[i].name, C(colour), 0);
        snprintf(text, sizeof(text), "%d", u->percent);
        set_text(W.gauge[i].fig, text);
        set_text(W.gauge[i].name, u->label);
    }

    /* the leg */
    int n = live ? W.model.n_hosts : 0;
    if (n == 0) { /* nothing to say: one plain run of frame */
        for (int i = 0; i < LEG_BLOCKS; i++) set_hidden(W.leg[i].block, i > 0);
        lv_obj_set_pos(W.leg[0].block, 0, LEG_Y);
        lv_obj_set_size(W.leg[0].block, LEG, LEG_H);
        lv_obj_set_style_bg_color(W.leg[0].block, C(C_TAN), 0);
        set_text(W.leg[0].fig, "");
        set_text(W.leg[0].name, "");
        return;
    }
    int h = (LEG_H - (n - 1) * GAP) / n, slot = 0;
    for (int i = 0; i < LEG_BLOCKS; i++) {
        bool used = i < n;
        lv_obj_t *b = W.leg[i].block;
        set_hidden(b, !used);
        if (!used) continue;
        int y = LEG_Y + slot * (h + GAP), bh = slot == n - 1 ? SH - y : h; /* the last block runs off the bottom edge */
        slot++;
        lv_obj_set_pos(b, 0, y);
        lv_obj_set_size(b, LEG, bh);
        int name_top = (slot == n ? h : bh) - LEG_PAD - cap_h(15); /* the host's name at the block's foot ... */
        lv_obj_set_y(W.leg[i].name, cap_at(15, name_top));
        lv_obj_set_y(W.leg[i].fig, cap_at(26, name_top - LEG_LINE - cap_h(26))); /* ... its count just above */

        const ac_host_t *hst = &W.model.hosts[i];
        bool selected = W.filter[0] && strcmp(W.filter, hst->name) == 0;
        lv_color_t colour;
        if (!hst->up) colour = C(C_GREY);
        else if (selected) colour = C(C_PALE);
        else if (hst->waiting > 0) colour = C(C_AMBER);
        else colour = shade(C_TAN, hst->sessions > 0 && !W.filter[0] ? 100 : 62);
        if (hst->up) snprintf(text, sizeof(text), "%d", hst->sessions);
        else snprintf(text, sizeof(text), "OFF");
        set_text(W.leg[i].fig, text);
        set_text(W.leg[i].name, hst->name);
        lv_obj_set_style_bg_color(b, colour, 0);
    }
}

static void update_clock(void) {
    char text[24];
    if (W.link != UI_LINK_OK || !W.have_model) { /* the clock's place says so, pulsing */
        set_text(W.clock, "NO LINK");
        set_text(W.date, "");
        lv_obj_set_style_text_color(W.clock, C(C_SALMON), 0);
        if (!lv_anim_get(W.clock, anim_text_opa)) pulse(W.clock, anim_text_opa, true);
        return;
    }
    if (lv_anim_get(W.clock, anim_text_opa)) {
        pulse(W.clock, anim_text_opa, false);
        lv_obj_set_style_text_opa(W.clock, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(W.clock, C(C_ROSE), 0);
    }
    format_clock(now_s(), "%H:%M", text, sizeof(text));
    set_text(W.clock, text);
    format_clock(now_s(), "%a %d %b", text, sizeof(text));
    set_text(W.date, text);
}

/* ---- the session list ----------------------------------------------------- */

static void row_clicked(lv_event_t *e) { ui_tap_row((int)(intptr_t)lv_event_get_user_data(e)); }

static void build_list(lv_obj_t *scr) {
    W.list = box(scr);
    lv_obj_set_pos(W.list, VIEW_X - ROW_PAD, VIEW_Y);
    lv_obj_set_size(W.list, VIEW_W + ROW_PAD, VIEW_H);
    lv_obj_add_flag(W.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(W.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(W.list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(W.list, C(C_TAN), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(W.list, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(W.list, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(W.list, 2, LV_PART_SCROLLBAR);

    for (int i = 0; i < AC_MAX_SESSIONS; i++) {
        row_t *r = &W.rows[i];
        r->session = -1;
        r->row = box(W.list);
        lv_obj_set_pos(r->row, 0, i * ROW_H);
        lv_obj_set_size(r->row, VIEW_W + ROW_PAD - ROW_END, ROW_H);
        lv_obj_set_style_radius(r->row, BAND_R, 0); /* of the band a tap gives it: style_row */
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(r->row, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        r->pill = pill(r->row, ROW_PAD, ROW_TOP, &r->pill_text, &r->glint);
        r->dur = label(r->row, F18, C_BLACK, "");
        lv_obj_set_width(r->dur, PILL_W);
        lv_obj_set_style_text_align(r->dur, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(r->dur, ROW_PAD, cap_at(18, ROW_TOP + PILL_H + 3));
        r->tok = label(r->row, F18, C_BLACK, "");
        lv_obj_set_width(r->tok, PILL_W);
        lv_obj_set_style_text_align(r->tok, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(r->tok, ROW_PAD, cap_at(18, ROW_TOP + PILL_H + 3));
        r->title = line(r->row, F26, C_BLACK, ROW_PAD + TEXT_X, cap_at(26, ROW_TOP + 1), TEXT_W - HOST_W - 10);
        r->host = line(r->row, F20, C_BLACK, ROW_PAD + TEXT_X + TEXT_W - HOST_W, cap_at(20, ROW_TOP + 1 + cap_h(26) - cap_h(20)), HOST_W);
        lv_obj_set_style_text_align(r->host, LV_TEXT_ALIGN_RIGHT, 0);
        r->sub = line(r->row, F20, C_BLACK, ROW_PAD + TEXT_X, cap_at(20, ROW_TOP + 1 + cap_h(26) + 8), TEXT_W);
    }
}

/* The tapped row lies on a band of its own colour; the others on black. */
static void style_row(lv_obj_t *row, bool tapped, uint32_t colour) {
    lv_obj_set_style_bg_color(row, shade(colour, BAND_BRIGHTNESS), 0);
    lv_obj_set_style_bg_opa(row, tapped ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void state_duration(const ac_session_t *s, char *out, size_t n) {
    if (s->since > 0) format_elapsed(now_s() - s->since, out, n);
    else out[0] = '\0';
}

/* Under the title: what is left for the person to do, once the session has stopped; failing that, what it waits for. */
static const char *sub_text(const ac_session_t *s) {
    if (s->todo[0]) return s->todo;
    return s->status == AC_WAITING ? s->detail : "";
}

/* Under the pill: how long the session has been in its state at the left, what it has spent at the right
 * ("2H4M ... 2.1M TOK"); either alone is centred. */
static void update_times(void) {
    char text[32];
    for (int i = 0; i < W.n_rows; i++) {
        const ac_session_t *s = &W.model.sessions[W.rows[i].session];
        state_duration(s, text, sizeof(text));
        bool both = text[0] && s->tokens[0];
        lv_obj_set_style_text_align(W.rows[i].dur, both ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(W.rows[i].tok, both ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER, 0);
        set_text(W.rows[i].dur, text);
        set_text(W.rows[i].tok, s->tokens);
    }
}

static void update_list(void) {
    int n = 0;
    for (int i = 0; i < W.model.n_sessions && W.have_model; i++) {
        const ac_session_t *s = &W.model.sessions[i];
        if (!shown(s)) continue;
        row_t *r = &W.rows[n++];
        r->session = i;
        uint32_t colour = STATUS[s->status].colour;
        bool quiet = s->quiet || s->status == AC_WORKING || s->status == AC_RUNNING; /* nothing the person can do */
        int bright = quiet ? QUIET_BRIGHTNESS : 100; /* the whole row */
        style_pill(r->pill, r->pill_text, s->status, bright);
        bool pulse = ui_status_pulses(s->status);
        if (pulse != r->pulsing) {
            set_pulse(r->pill, pulse);
            r->pulsing = pulse;
        }
        bool glint = s->status == AC_WORKING;
        if (glint != r->glinting) {
            set_glint(r->glint, glint);
            r->glinting = glint;
        }
        lv_obj_set_style_text_color(r->dur, shade(colour, SUB_BRIGHTNESS * bright / 100), 0);
        lv_obj_set_style_text_color(r->tok, shade(colour, SUB_BRIGHTNESS * bright / 100), 0);
        lv_obj_set_style_text_color(r->title, shade(colour, bright), 0);
        set_text(r->title, s->title);
        lv_obj_set_style_text_color(r->host, shade(colour, SUB_BRIGHTNESS * bright / 100), 0);
        set_text(r->host, s->host);
        bool tapped = W.tapped[0] && strcmp(s->key, W.tapped) == 0;
        style_row(r->row, tapped, colour);
        if (tapped) { /* what became of the tap, where the finger was */
            lv_obj_set_style_text_color(r->sub, C(W.tap_note.colour), 0);
            set_text(r->sub, W.tap_note.text);
        } else {
            bool todo = s->todo[0] && strcmp(s->todo, TODO_PENDING) != 0; /* an answer, not the hub still asking */
            lv_obj_set_style_text_color(r->sub, todo ? shade(C_PALE, bright) : shade(colour, SUB_BRIGHTNESS * bright / 100), 0);
            set_text(r->sub, sub_text(s));
        }
        set_hidden(r->row, false);
    }
    for (int i = n; i < AC_MAX_SESSIONS; i++) {
        row_t *r = &W.rows[i];
        if (r->pulsing) {
            set_pulse(r->pill, false);
            r->pulsing = false;
        }
        if (r->glinting) {
            set_glint(r->glint, false);
            r->glinting = false;
        }
        r->session = -1;
        set_hidden(r->row, true);
    }
    W.n_rows = n;
    update_times();
}

/* ---- the message panel: link states and the empty fleet ---------------------- */

static void build_message(lv_obj_t *scr) {
    W.message = box(scr);
    lv_obj_set_pos(W.message, VIEW_X, VIEW_Y);
    lv_obj_set_size(W.message, VIEW_W, VIEW_H);
    W.message_title = label(W.message, F34, C_TAN, "");
    lv_obj_set_width(W.message_title, VIEW_W - 14);
    lv_obj_set_style_text_align(W.message_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(W.message_title, 0, cap_at(34, 140));
    W.message_detail = label(W.message, F20, C_LAV, "");
    lv_obj_set_width(W.message_detail, VIEW_W - 14);
    lv_obj_set_style_text_align(W.message_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(W.message_detail, 0, cap_at(20, 140 + cap_h(34) + 16));
}

/* ---- the "+": a new session ---------------------------------------------------- */

static void plus_clicked(lv_event_t *e) {
    (void)e;
    ui_tap_new();
}

static void build_plus(lv_obj_t *scr) {
    /* a round key over the list's bottom right corner; a ring of black parts it from the rows passing under it */
    W.plus = fill(scr, C_TAN, SW - PLUS_MARGIN - PLUS_D, SH - PLUS_MARGIN - PLUS_D, PLUS_D, PLUS_D);
    lv_obj_set_style_radius(W.plus, PLUS_D / 2, 0);
    lv_obj_set_style_outline_color(W.plus, C(C_BLACK), 0);
    lv_obj_set_style_outline_width(W.plus, GAP, 0);
    lv_obj_set_style_outline_pad(W.plus, 0, 0);
    lv_obj_add_flag(W.plus, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(W.plus, PLUS_MARGIN);
    lv_obj_add_event_cb(W.plus, plus_clicked, LV_EVENT_CLICKED, NULL);
    fill(W.plus, C_BLACK, (PLUS_D - PLUS_BAR_L) / 2, (PLUS_D - PLUS_BAR_T) / 2, PLUS_BAR_L, PLUS_BAR_T);
    fill(W.plus, C_BLACK, (PLUS_D - PLUS_BAR_T) / 2, (PLUS_D - PLUS_BAR_L) / 2, PLUS_BAR_T, PLUS_BAR_L);

    /* what became of its request, to its left: one line sits level with it, a second grows upward */
    W.plus_note = label(scr, F20, C_TAN, "");
    lv_obj_set_style_bg_color(W.plus_note, C(C_BLACK), 0);
    lv_obj_set_style_bg_opa(W.plus_note, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(W.plus_note, NOTE_PAD_X, 0);
    lv_obj_set_style_pad_ver(W.plus_note, NOTE_PAD_Y, 0);
    lv_obj_set_style_radius(W.plus_note, BAND_R, 0);
    lv_obj_set_style_text_align(W.plus_note, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_line_space(W.plus_note, -4, 0);
    lv_obj_set_style_max_width(W.plus_note, VIEW_W - PLUS_D - 2 * PLUS_MARGIN - GAP, 0);
    int line_h = lv_font_get_line_height(F20), ink_mid = cap_top(20) + cap_h(20) / 2; /* capitals sit low in the line box */
    lv_obj_align(W.plus_note, LV_ALIGN_BOTTOM_RIGHT, -(PLUS_MARGIN + PLUS_D + GAP + PLUS_MARGIN - NOTE_PAD_X),
                 -(PLUS_MARGIN + PLUS_D / 2 - (line_h - ink_mid) - NOTE_PAD_Y));
}

/* The "+" is there whenever the hub is: it is the hub that opens the session. */
static void update_plus(void) {
    bool live = W.have_model && W.link == UI_LINK_OK;
    if (!live) note_clear(&W.new_note); /* its answer, if one comes, starts a note of its own */
    set_hidden(W.plus, !live);
    set_hidden(W.plus_note, !live || !W.new_note.text[0]);
    lv_obj_set_style_text_color(W.plus_note, C(W.new_note.colour), 0);
    set_text(W.plus_note, W.new_note.text);
    bool pulsing = lv_anim_get(W.plus, anim_bg_opa) != NULL;
    if (W.new_note.pending != pulsing) {
        set_pulse(W.plus, W.new_note.pending);
        if (!W.new_note.pending) lv_obj_set_style_bg_opa(W.plus, LV_OPA_COVER, 0);
    }
}

/* ---- which panel shows -------------------------------------------------------- */

static void update_view(void) {
    const char *title = NULL, *detail = "";
    char text[48];
    if (W.link == UI_LINK_WIFI) title = "JOINING WI-FI";
    else if (W.link == UI_LINK_ERROR) title = "HUB ERROR";
    else if (W.link == UI_LINK_LOST) title = "NO LINK TO THE HUB";
    else if (W.link == UI_LINK_FETCHING || !W.have_model) title = "CONTACTING THE HUB";
    else if (W.n_rows == 0) {
        if (W.filter[0]) {
            snprintf(text, sizeof(text), "NO AGENTS ON %s", W.filter);
            title = text;
        } else {
            title = "NO LIVE AGENTS";
        }
    }
    if (title) {
        set_text(W.message_title, title);
        if (W.link == UI_LINK_OK) set_text(W.message_detail, detail);
    }
    set_hidden(W.message, !title);
    set_hidden(W.list, title != NULL);
}

static void refresh(void) {
    if (W.filter[0]) { /* a filter on a host the hub no longer lists would hide everything for good */
        bool known = false;
        for (int i = 0; i < W.model.n_hosts; i++) known |= strcmp(W.model.hosts[i].name, W.filter) == 0;
        if (!known) W.filter[0] = '\0';
    }
    if (W.tapped[0]) { /* the tapped session has ended: its note has no row to be printed in */
        bool live = false;
        for (int i = 0; i < W.model.n_sessions; i++) live |= strcmp(W.model.sessions[i].key, W.tapped) == 0;
        if (!live) {
            W.tapped[0] = '\0';
            note_clear(&W.tap_note);
        }
    }
    update_frame();
    update_clock();
    update_list();
    update_plus();
    update_view();
}

static void tick(lv_timer_t *timer) {
    (void)timer;
    if (note_over(&W.tap_note)) { /* the tapped row goes back to being one of the list */
        W.tapped[0] = '\0';
        note_clear(&W.tap_note);
        update_list();
    }
    if (note_over(&W.new_note)) {
        note_clear(&W.new_note);
        update_plus();
    }
    int minute = (int)(now_s() / 60);
    if (minute == W.last_minute) return;
    W.last_minute = minute;
    update_clock();
    update_times();
}

/* ---- public ------------------------------------------------------------------- */

void ui_init(const void *ttf, size_t ttf_len) {
    F15 = lv_tiny_ttf_create_data_ex(ttf, ttf_len, 15, LV_FONT_KERNING_NORMAL, 32);
    F18 = lv_tiny_ttf_create_data_ex(ttf, ttf_len, 18, LV_FONT_KERNING_NORMAL, 96);
    F20 = lv_tiny_ttf_create_data_ex(ttf, ttf_len, 20, LV_FONT_KERNING_NORMAL, 64);
    F26 = lv_tiny_ttf_create_data_ex(ttf, ttf_len, 26, LV_FONT_KERNING_NORMAL, 96);
    F34 = lv_tiny_ttf_create_data_ex(ttf, ttf_len, 34, LV_FONT_KERNING_NORMAL, 64);
    F44 = lv_tiny_ttf_create_data_ex(ttf, ttf_len, 44, LV_FONT_KERNING_NORMAL, 32);
    (void)font_px;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, C(C_BLACK), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    W.link = UI_LINK_WIFI;
    W.last_minute = -1;
    build_frame(scr);
    build_list(scr);
    build_message(scr);
    build_plus(scr);
    refresh();
    lv_timer_create(tick, 1000, NULL);
}

void ui_set_model(const ac_model_t *model) {
    W.model = *model;
    W.have_model = true;
    W.model_tick = lv_tick_get();
    W.link = UI_LINK_OK;
    W.last_minute = (int)(now_s() / 60);
    refresh();
}

void ui_set_link(ui_link_t link, const char *detail) {
    W.link = link;
    char last[32] = "";
    if (link == UI_LINK_LOST && W.have_model && !detail) { /* the hub's own clock at its last answer */
        format_clock(W.model.now, "LAST CONTACT %H:%M", last, sizeof(last));
        detail = last;
    }
    set_text(W.message_detail, detail ? detail : "");
    refresh();
}

uint32_t ui_status_colour(ac_status_t status) { return STATUS[status].colour; }

bool ui_status_pulses(ac_status_t status) { return status == AC_WAITING; }

bool ui_attention(void) {
    if (!W.have_model || W.link != UI_LINK_OK) return false;
    for (int i = 0; i < W.model.n_sessions; i++) {
        ac_status_t s = W.model.sessions[i].status;
        if (s == AC_WORKING || s == AC_WAITING || s == AC_RUNNING || s == AC_DONE || s == AC_FAILED) return true;
    }
    return false;
}

void ui_tap_row(int row) {
    if (row < 0 || row >= W.n_rows) return;
    const ac_session_t *s = &W.model.sessions[W.rows[row].session];
    char text[48];
    snprintf(W.tapped, sizeof(W.tapped), "%s", s->key);
    snprintf(text, sizeof(text), "OPENING ON %s", s->host);
    note_set(&W.tap_note, true, C_PALE, REQUEST_MS, text);
    update_list();
    if (W.focus_handler) W.focus_handler(W.tapped);
}

void ui_set_focus_handler(ui_focus_handler_t handler) { W.focus_handler = handler; }

void ui_set_focus_result(const char *key, bool ok, const char *message) {
    if (strcmp(key, W.tapped) != 0) return; /* another row has been tapped since: nowhere to say it */
    note_set(&W.tap_note, false, ok ? C_GREEN : C_AMBER, ok ? DONE_MS : REFUSED_MS, message);
    update_list();
}

void ui_tap_new(void) {
    if (lv_obj_has_flag(W.plus, LV_OBJ_FLAG_HIDDEN) || W.new_note.pending) return; /* one at a time: a second tap would open a second session */
    note_set(&W.new_note, true, C_PALE, REQUEST_MS, "OPENING A NEW SESSION");
    update_plus();
    if (W.new_handler) W.new_handler();
}

void ui_set_new_handler(ui_new_handler_t handler) { W.new_handler = handler; }

void ui_set_new_result(bool ok, const char *message) {
    note_set(&W.new_note, false, ok ? C_GREEN : C_AMBER, ok ? DONE_MS : REFUSED_MS, message);
    update_plus();
}

void ui_set_filter(const char *host) {
    snprintf(W.filter, sizeof(W.filter), "%s", host ? host : "");
    lv_obj_scroll_to_y(W.list, 0, LV_ANIM_OFF);
    refresh();
}
