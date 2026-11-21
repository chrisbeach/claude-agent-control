#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "board";

/* Pin map from Waveshare's ESP-IDF demo (08_lvgl_Porting/waveshare_rgb_lcd_port.h). */
#define PIN_I2C_SDA GPIO_NUM_8
#define PIN_I2C_SCL GPIO_NUM_9
#define PIN_TOUCH_INT GPIO_NUM_4
#define PIN_LCD_VSYNC GPIO_NUM_3
#define PIN_LCD_HSYNC GPIO_NUM_46
#define PIN_LCD_DE GPIO_NUM_5
#define PIN_LCD_PCLK GPIO_NUM_7
/* B3..B7, G2..G7, R3..R7 */
#define LCD_DATA_PINS {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40}

/* AC_ROTATE_180=1 (secrets.ini `rotate_180`) turns the picture and the touch
 * coordinates upside down, for a board mounted with its USB sockets the other way. */
#ifndef AC_ROTATE_180
#define AC_ROTATE_180 0
#endif

#define LCD_PIXEL_CLOCK_HZ (16 * 1000 * 1000)
#define LCD_BOUNCE_BUFFER_LINES 10

/* CH422G: writing to address 0x24 sets the mode register (0x01 = all EXIO as push-pull
 * outputs); writing to 0x38 sets the eight output levels. */
#define CH422G_ADDR_MODE 0x24
#define CH422G_ADDR_OUT 0x38
#define CH422G_MODE_OUTPUT 0x01
#define EXIO_TOUCH_RESET (1 << 1)
#define EXIO_BACKLIGHT (1 << 2)
#define EXIO_LCD_RESET (1 << 3)
#define EXIO_SD_CS (1 << 4)
#define EXIO_USB_SEL (1 << 5) /* low selects USB on the shared pins */

static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t ch422g_mode, ch422g_out;
static uint8_t exio_levels = EXIO_TOUCH_RESET | EXIO_BACKLIGHT | EXIO_LCD_RESET | EXIO_SD_CS;
static esp_lcd_panel_handle_t panel;
static esp_lcd_touch_handle_t touch;
static lv_display_t *display;

static esp_err_t ch422g_write(i2c_master_dev_handle_t dev, uint8_t value) {
    return i2c_master_transmit(dev, &value, 1, 100);
}

static esp_err_t ch422g_set(uint8_t mask, bool high) {
    if (high) exio_levels |= mask;
    else exio_levels &= ~mask;
    return ch422g_write(ch422g_out, exio_levels);
}

static esp_err_t i2c_init(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c_bus), TAG, "i2c bus");

    i2c_device_config_t mode_cfg = {.device_address = CH422G_ADDR_MODE, .scl_speed_hz = 400000};
    i2c_device_config_t out_cfg = {.device_address = CH422G_ADDR_OUT, .scl_speed_hz = 400000};
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &mode_cfg, &ch422g_mode), TAG, "ch422g mode");
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &out_cfg, &ch422g_out), TAG, "ch422g out");
    ESP_RETURN_ON_ERROR(ch422g_write(ch422g_mode, CH422G_MODE_OUTPUT), TAG, "ch422g set output mode");
    return ESP_OK;
}

/* GT911 picks its I2C address at reset: INT held low -> 0x5D. */
static esp_err_t touch_reset(void) {
    gpio_config_t int_out = {.pin_bit_mask = 1ULL << PIN_TOUCH_INT, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&int_out), TAG, "touch int gpio");
    gpio_set_level(PIN_TOUCH_INT, 0);
    ESP_RETURN_ON_ERROR(ch422g_set(EXIO_TOUCH_RESET, false), TAG, "touch reset low");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(ch422g_set(EXIO_TOUCH_RESET, true), TAG, "touch reset high");
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_TOUCH_INT, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_config_t int_in = {.pin_bit_mask = 1ULL << PIN_TOUCH_INT, .mode = GPIO_MODE_INPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&int_in), TAG, "touch int input");
    return ESP_OK;
}

static esp_err_t panel_init(void) {
    const int data_pins[16] = LCD_DATA_PINS;
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PIXEL_CLOCK_HZ,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        .bounce_buffer_size_px = BOARD_LCD_H_RES * LCD_BOUNCE_BUFFER_LINES,
        .dma_burst_size = 64,
        .hsync_gpio_num = PIN_LCD_HSYNC,
        .vsync_gpio_num = PIN_LCD_VSYNC,
        .de_gpio_num = PIN_LCD_DE,
        .pclk_gpio_num = PIN_LCD_PCLK,
        .disp_gpio_num = -1,
        .flags.fb_in_psram = true,
    };
    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data_pins[i];
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &panel), TAG, "rgb panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    return ESP_OK;
}

static esp_err_t touch_init(void) {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &io), TAG, "touch io");

    esp_lcd_touch_config_t cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC, /* reset is on the expander, done in touch_reset() */
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = AC_ROTATE_180, .mirror_y = AC_ROTATE_180},
    };
    return esp_lcd_touch_new_i2c_gt911(io, &cfg, &touch);
}

#if AC_ROTATE_180
/* The RGB panel has no controller to flip the picture, and the driver's mirror
 * flags do nothing when LVGL draws straight into the panel's frame buffers. So
 * LVGL draws into a canvas of its own, and each finished frame is copied, pixel
 * order reversed, into the panel's hidden frame buffer, which is then swapped in
 * at the next frame boundary: still no tearing. */
static uint16_t *canvas;
static uint16_t *panel_fbs[2];
static int back_fb = 1;
static SemaphoreHandle_t frame_done;
static lv_area_t frame_dirty, prev_dirty;
static bool frame_has_dirty, prev_has_dirty;

static IRAM_ATTR bool on_frame_done(esp_lcd_panel_handle_t p, const esp_lcd_rgb_panel_event_data_t *e, void *ctx) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &woken);
    return woken == pdTRUE;
}

static void copy_rotated(uint16_t *fb, const lv_area_t *a) {
    int n = a->x2 - a->x1 + 1;
    for (int y = a->y1; y <= a->y2; y++) {
        const uint16_t *src = canvas + y * BOARD_LCD_H_RES + a->x1;
        uint16_t *dst = fb + (BOARD_LCD_V_RES - 1 - y) * BOARD_LCD_H_RES + (BOARD_LCD_H_RES - 1 - a->x1);
        for (int i = 0; i < n; i++) *dst-- = *src++;
    }
}

static void area_join(lv_area_t *a, const lv_area_t *b) {
    if (b->x1 < a->x1) a->x1 = b->x1;
    if (b->y1 < a->y1) a->y1 = b->y1;
    if (b->x2 > a->x2) a->x2 = b->x2;
    if (b->y2 > a->y2) a->y2 = b->y2;
}

static void rotated_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    if (frame_has_dirty) {
        area_join(&frame_dirty, area);
    } else {
        frame_dirty = *area;
        frame_has_dirty = true;
    }
    if (lv_display_flush_is_last(disp)) {
        /* The hidden buffer is one frame behind, so it also needs what the
         * previous frame changed. */
        lv_area_t todo = frame_dirty;
        if (prev_has_dirty) area_join(&todo, &prev_dirty);
        copy_rotated(panel_fbs[back_fb], &todo);
        esp_lcd_panel_draw_bitmap(panel, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, panel_fbs[back_fb]);
        /* Wait until the panel has finished with the buffer it was showing. */
        xSemaphoreTake(frame_done, 0);
        xSemaphoreTake(frame_done, portMAX_DELAY);
        back_fb ^= 1;
        prev_dirty = frame_dirty;
        prev_has_dirty = true;
        frame_has_dirty = false;
    }
    lv_display_flush_ready(disp);
}

static void wake_lvgl_task(lv_event_t *e) {
    lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
}

static lv_display_t *add_rotated_display(void) {
    size_t bytes = BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t);
    canvas = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);
    frame_done = xSemaphoreCreateBinary();
    if (!canvas || !frame_done) return NULL;
    if (esp_lcd_rgb_panel_get_frame_buffer(panel, 2, (void **)&panel_fbs[0], (void **)&panel_fbs[1]) != ESP_OK) return NULL;
    const esp_lcd_rgb_panel_event_callbacks_t cbs = {.on_frame_buf_complete = on_frame_done};
    if (esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, frame_done) != ESP_OK) return NULL;

    lvgl_port_lock(0);
    lv_display_t *disp = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    if (disp) {
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(disp, canvas, NULL, bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
        lv_display_set_flush_cb(disp, rotated_flush);
        lv_display_add_event_cb(disp, wake_lvgl_task, LV_EVENT_REFR_REQUEST, NULL);
    }
    lvgl_port_unlock();
    return disp;
}
#else
static lv_display_t *add_direct_display(void) {
    /* Direct mode on the panel's own two PSRAM frame buffers: no tearing, no copy. */
    lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = panel,
        .buffer_size = BOARD_LCD_H_RES * BOARD_LCD_V_RES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .flags = {.buff_dma = false, .buff_spiram = true, .direct_mode = true, .swap_bytes = false},
    };
    lvgl_port_display_rgb_cfg_t rgb_cfg = {.flags = {.bb_mode = true, .avoid_tearing = true}};
    return lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
}
#endif

static esp_err_t lvgl_init(void) {
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 1;
    port_cfg.task_stack = 12 * 1024;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

#if AC_ROTATE_180
    display = add_rotated_display();
#else
    display = add_direct_display();
#endif
    ESP_RETURN_ON_FALSE(display, ESP_FAIL, TAG, "add display");

    if (touch) {
        lvgl_port_touch_cfg_t touch_cfg = {.disp = display, .handle = touch};
        ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&touch_cfg), ESP_FAIL, TAG, "add touch");
    }
    return ESP_OK;
}

esp_err_t board_init(void) {
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "i2c");
    /* Backlight off until the first screen is drawn, so there is no white flash. */
    ESP_RETURN_ON_ERROR(ch422g_set(EXIO_BACKLIGHT, false), TAG, "backlight");
    ESP_RETURN_ON_ERROR(touch_reset(), TAG, "touch reset");
    ESP_RETURN_ON_ERROR(panel_init(), TAG, "panel");
    /* A touch panel that does not answer (its ribbon cable knocked loose, say) must not
     * take the whole display down: carry on without touch, just no wake-on-touch. */
    if (touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "touch controller not responding; running without touch");
        touch = NULL;
    }
    ESP_RETURN_ON_ERROR(lvgl_init(), TAG, "lvgl");
    ESP_LOGI(TAG, "display ready%s", touch ? "" : " (no touch)");
    return ESP_OK;
}

bool board_touch_available(void) {
    return touch != NULL;
}

esp_err_t board_backlight(bool on) {
    return ch422g_set(EXIO_BACKLIGHT, on);
}

lv_display_t *board_display(void) {
    return display;
}
