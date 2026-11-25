/* Waveshare ESP32-S3-Touch-LCD-7: 800x480 RGB panel, GT911 touch, CH422G IO expander. */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#define BOARD_LCD_H_RES 800
#define BOARD_LCD_V_RES 480

/* Brings up I2C, the expander, the panel, touch and the LVGL port task. A touch
 * controller that does not answer is logged and skipped, not fatal. */
esp_err_t board_init(void);

bool board_touch_available(void);

/* The backlight is a plain on/off line on the expander (no dimming). */
esp_err_t board_backlight(bool on);
bool board_backlight_on(void); /* as last set */

lv_display_t *board_display(void);
