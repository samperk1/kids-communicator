#pragma once
/* Screen capture over the USB console, for making README videos from the
 * handset's own pixels. 
 *
 * 🔑 BUILD SWITCH, OFF BY DEFAULT. Only compiled when CMake is given
 * -DSCREEN_CAPTURE=ON (its own build dir, e.g. build-capture). The everyday
 * firmware has none of this code, and every call site is #if SCREEN_CAPTURE.
 *
 * Nothing is streamed until the PC sends 'F' (tools/screencap.py does on
 * connect), so a capture build left on a desk with no recorder never stalls
 * the UI on a full USB buffer. */
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#if SCREEN_CAPTURE
void screencap_init(void);    /* after lv_init(), from the LVGL task */
void screencap_region(const lv_area_t *a, const lv_color_t *px);
void screencap_touch(int x, int y, bool pressed);
#endif
