#pragma once

/* ------------------------------------------------------------------ *
 * LVGL's object pool, moved to PSRAM.
 *
 * 🚨 WHY THIS EXISTS: every lv_obj, style and animation was coming out of
 * INTERNAL RAM, because LV_MEM_CUSTOM routes to plain malloc and
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL is 16384 — so every small LVGL
 * allocation lands internal. Measured cost on this board: ~600 bytes per
 * object, ~18KB for one game screen, against ~27KB of internal headroom while
 * music is playing. The player then could not get a DMA-capable buffer to open
 * the next track:
 *
 *     E dma_utils: esp_dma_capable_malloc(172): Not enough heap memory
 *     E play: cannot open /sdcard/music/.../18 Belle.mp3
 *
 * ⚠️ THIS IS SAFE ONLY BECAUSE THE DRAW BUFFERS DO NOT COME FROM HERE.
 * display.c allocates those itself with heap_caps_malloc(MALLOC_CAP_DMA), and
 * they must stay that way — spi_device_polling_transmit still uses DMA, so a
 * PSRAM draw buffer crashes. LV_MEM only holds objects, styles and animations,
 * none of which is ever handed to the SPI peripheral.
 *
 * Reached via CONFIG_LV_MEM_CUSTOM_INCLUDE="lv_psram.h". lv_mem.c includes it
 * at line 17 and uses the macros at 136/183/208, so redefining them here
 * takes effect. lv_mem.c is compiled inside the LVGL component, so
 * main/CMakeLists.txt adds this directory to THAT component's include path.
 * ------------------------------------------------------------------ */

#include <stdlib.h>
#include "esp_heap_caps.h"

/* Fall back to internal if PSRAM is exhausted: a UI that allocates slowly is
 * better than one that returns NULL and asserts. */
static inline void *lv_psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static inline void *lv_psram_realloc(void *p, size_t n)
{
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n);
}

/* heap_caps_free handles pointers from either heap, so the fallback above does
 * not need tracking. */
#undef LV_MEM_CUSTOM_ALLOC
#undef LV_MEM_CUSTOM_FREE
#undef LV_MEM_CUSTOM_REALLOC
#define LV_MEM_CUSTOM_ALLOC   lv_psram_alloc
#define LV_MEM_CUSTOM_FREE    heap_caps_free
#define LV_MEM_CUSTOM_REALLOC lv_psram_realloc
