#pragma once
#include <stdbool.h>

void battery_init(void);
bool battery_fitted(void);       /* false until the pack is wired: see hw.h */
/* Filtered pack volts in mV, or -1. Slow exponential average: one percent of
 * charge is only 9mV, so the raw ADC visibly flickers the meter. Use this for
 * anything the user sees or that decides a low-battery warning. */
int  battery_millivolts(void);
/* Unfiltered instantaneous reading. Only for "is a pack present" and for
 * diagnostics - a warning built on this will fire on the amp's load sag. */
int  battery_millivolts_raw(void);
int  battery_percent(void);      /* 0-100, or -1 */
bool battery_charging(void);
/* True when a USB HOST is connected - read from the S3's USB peripheral, not
 * from the ADC, because on USB with no pack GPIO9 floats at ~4150mV and is
 * indistinguishable from a full battery. ⚠️ A dumb charger provides no data and
 * will read false; this is host detection, not charger detection. */
bool battery_on_usb(void);
/* True when the pack is genuinely low (filtered voltage, and never while on
 * USB). Drives the low-battery warning - see ui.c. */
bool battery_low(void);
