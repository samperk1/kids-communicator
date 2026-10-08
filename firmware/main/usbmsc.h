#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* USB card mode — the SD card appears on the PC as a USB drive.
 *
 * 🔑 WHY IT EXISTS. The builder, 2026-09-13: *"i can pull the card but that may mean
 * cutting the power to the device and that would leave us starting over with
 * the gps"*, and *"i didn't want the kids to pull them and lose the cards"*, so
 * the slot is deliberately hard to reach. Getting 37 MB of map tiles — or an
 * album — onto the card had no route that did not involve a screwdriver and a
 * power cut.
 *
 * ⚠️ THE HANDSET'S USB CONSOLE DISAPPEARS WHILE THIS IS ON. The ESP32-S3 has
 * two separate USB blocks sharing one pair of pins: USB-Serial-JTAG (which is
 * the console and the flashing port) and USB-OTG (which TinyUSB drives). Only
 * one can own the pins, so /dev/ttyACM0 goes away and comes back as a disk.
 * A reboot puts the console back — which is why the only way out is a reboot.
 *
 * 🔑 AND A REBOOT DOES NOT COST THE GPS ITS ALMANAC. The module's 3V3 rail is
 * not switched by the ESP32, measured across a whole day of reflashing on
 * 2026-09-12. Pulling the card would have. That is the entire point. */

/* True once the card belongs to the PC. */
bool usbmsc_active(void);

/* Hand the card to the PC. Stops everything that reads the card first.
 * There is no way back except esp_restart(). */
esp_err_t usbmsc_enter(void);

/* One line for the screen: what happened, or why it did not. Never NULL. */
const char *usbmsc_status(void);

/* Is a USB host actually there?
 *
 * 🚨 DO NOT USE battery_on_usb() FOR THIS. It is usb_serial_jtag_is_connected(),
 * and card mode is exactly the state in which that peripheral is dead — TinyUSB
 * has taken the pins. Measured 2026-09-13: the unplug-detector built on it fired
 * the instant card mode started, so the card mounted on the PC and the handset
 * rebooted out from under it every single time. The mode destroyed the signal
 * that was guarding it.
 *
 * TinyUSB's own tud_mounted() is the authority once TinyUSB owns the port.
 * Returns true before the host has ever enumerated, so a slow enumeration is
 * never mistaken for a cable that was pulled. */
bool usbmsc_host_present(void);

/* Give the USB hardware back before restarting.
 *
 * 🚨 THIS IS WHAT esp_restart() ALONE DOES NOT DO, AND IT IS WHY CARD MODE USED
 * TO NEED THE RESET BUTTON WITH THE CASE OPEN. tinyusb_driver_install() moves
 * the USB pins from the Serial/JTAG controller to the OTG controller, and that
 * selection SURVIVES a software restart — esp_restart() resets the CPU, not the
 * chip. Only the hardware reset re-runs the ROM, which re-selects the pad. So
 * the handset came back up running perfectly and completely invisible to the PC.
 *
 * tinyusb_driver_uninstall() -> usb_del_phy() releases it. Call this BEFORE
 * esp_restart() and the console comes back on its own. */
esp_err_t usbmsc_release(void);

/* Call once at boot, before anything else wants the USB console. Undoes a card
 * mode session that never released the pad — the recovery that does NOT depend
 * on the exit path having worked. */
void usbmsc_boot_recover(void);
