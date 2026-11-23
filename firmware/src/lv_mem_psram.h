/* LVGL's allocator: its whole heap in PSRAM (lv_mem_psram.c). */
#pragma once

#include <stddef.h>

/* What LVGL holds now, in bytes, and how many of its blocks had to go to internal RAM
 * because PSRAM had no room (0 on a healthy board): both for /status. */
size_t lv_mem_psram_bytes(void);
size_t lv_mem_psram_fallbacks(void);
