/* LVGL's heap, in PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC): every object, label text, style and,
 * above all, the fonts' glyph caches. On plain malloc each of those small blocks goes to internal
 * RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL), and the glyph caches keep filling as new characters
 * reach the screen, until Wi-Fi and lwIP, which can only use internal RAM, are left with a few KB.
 * Out here they have 8 MB to grow into. The frame buffers are in PSRAM already, and the panel is
 * fed through internal bounce buffers (board.c), so the extra PSRAM reads don't disturb the picture. */

#include "lv_mem_psram.h"

#include <stdlib.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM
#error "lv_mem_psram.c is LVGL's allocator: set CONFIG_LV_USE_CUSTOM_MALLOC=y"
#endif

#define CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static size_t s_bytes;     /* live, as heap_caps counts a block's usable size */
static size_t s_fallbacks; /* blocks that went to internal RAM because PSRAM had none */

static void *counted(void *p, bool fallback) {
    if (p) {
        s_bytes += heap_caps_get_allocated_size(p);
        s_fallbacks += fallback;
    }
    return p;
}

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size) {
    void *p = heap_caps_malloc(size, CAPS);
    return p ? counted(p, false) : counted(malloc(size), true);
}

void *lv_realloc_core(void *p, size_t new_size) {
    if (!p) return lv_malloc_core(new_size);
    size_t old = heap_caps_get_allocated_size(p);
    void *q = heap_caps_realloc(p, new_size, CAPS);
    bool fallback = false;
    if (!q) {
        q = realloc(p, new_size);
        fallback = true;
    }
    if (!q) return NULL; /* p is untouched and still counted */
    s_bytes -= old;
    return counted(q, fallback);
}

void lv_free_core(void *p) {
    if (!p) return;
    s_bytes -= heap_caps_get_allocated_size(p);
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { LV_UNUSED(mon_p); }

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

size_t lv_mem_psram_bytes(void) { return s_bytes; }

size_t lv_mem_psram_fallbacks(void) { return s_fallbacks; }
