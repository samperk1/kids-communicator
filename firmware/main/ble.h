#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Bring up NimBLE and start the standby profile: a low-duty advert plus a
 * duty-cycled scan. Logs internal-RAM headroom at every stage — that is the
 * point of it. Safe to not call at all; nothing else depends on it. */
void ble_init(void);

/* Advertise a "call me" burst for a few seconds. Returns false if BLE is not
 * running. */
bool ble_chirp(void);

/* True once per received chirp, with the sender's comm_XXXX id. Poll it from
 * the UI thread — the BLE host task must never touch LVGL. */
bool ble_take_chirp(char *id_out, size_t len);

/* 🔑 BLE AND WIFI CANNOT BOTH BE RESIDENT — see the measurements in ble.c.
 * net_up() calls ble_suspend() and net_down() calls ble_resume(), so the two
 * radios take turns. Both are no-ops if BLE was never started. */
void ble_suspend(void);
void ble_resume(void);
