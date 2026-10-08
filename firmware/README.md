# Firmware

ESP-IDF firmware for the handset (Hosyond/LCDwiki ES3C28P: ESP32-S3, 2.8" ILI9341
touch screen, 16 MB flash, 8 MB PSRAM).

This is the **credential-free build**. It holds no WiFi password and no API token,
and it opens no network sockets: handsets talk to each other directly over ESP-NOW,
encrypted per link with keys derived from a fleet code you type in once per handset.

## Build

Built and tested on **ESP-IDF 5.3**.

```sh
. $IDF_PATH/export.sh
cd firmware
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The first build downloads LVGL 8.4 and the USB stack from the ESP component
registry. There is nothing to configure and no secrets file to create.

⚠️ If you change `sdkconfig.defaults`, delete the generated `sdkconfig` before
rebuilding. A stale `sdkconfig` silently wins over the defaults.

## First boot

1. Touch calibration runs if the board has none stored.
2. **Settings → Name**: pick a name for whoever is holding this handset.
3. **Fleet code**: enter the same 8-digit code on every handset in your family.
   It never leaves the handset, and it is the root of the encryption.
4. **Pair** each handset with the others from the Fleet screen.

## Layout

| Path | What |
|---|---|
| `main/` | All firmware source |
| `main/light_stubs.c` | Answers the network API with honest failures. This build has no WiFi client |
| `main/lvgl_hook/lv_psram.h` | Moves LVGL's object pool to PSRAM (put on LVGL's include path by `main/CMakeLists.txt`) |
| `main/sprites/` | Game sprites, **generated**: edit `art/sprites.py` and run `tools/png2lvgl.py` |
| `partitions.csv` | 6 MB app, plus a 1 MB data partition for the predictive-text dictionary |

## Not here yet

- **The predictive-text dictionary.** `idf.py flash` does not write the `t9dic`
  partition, and the dictionary and its build script are not published yet. Without
  it the keypad still works; it just shows digits instead of suggesting words.

The comments in the source are long on purpose. Most of them record why something
is the way it is, usually because the obvious version was tried on real hardware
and failed.
