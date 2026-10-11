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

⚠️ **Tap the screen after plugging in.** A handset that went to sleep on
battery does not wake when you plug it in: deep sleep switches USB off, and the
board has no way to notice USB power arriving. If `/dev/ttyACM0` is missing,
touch the screen. Once awake it never sleeps while connected to a computer.

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
| `tools/build_t9_dict.py`, `data/t9.dic` | The predictive-text dictionary and how it is built |

## Predictive text

`idf.py flash` does not write the dictionary; it lives in its own partition.
Write it once per handset:

```sh
tools/flash_t9.sh /dev/ttyACM0
```

Without it the keypad still works, but shows digits instead of suggesting words.

`data/t9.dic` is ready-built. `tools/build_t9_dict.py` rebuilds it from Debian's
`wamerican` word list (`/usr/share/dict/american-english`), and **filters it for
a child's device**: swear words and slurs are removed using
`tools/t9_blocklist.txt` plus `tools/t9_blocklist_extra.txt`, while
`tools/t9_allowlist.txt` keeps everyday words the filter would otherwise catch
("butter", "sucks", "xx"). Built with `wamerican` 2020.12.07; a different version
of the word list gives a slightly different file.

**Names** are not in the dictionary. Tap **ABC** on the keypad to spell a word
letter by letter (press a key, then tap the letter you want; the first letter is
a capital). The word is saved on that handset and offered as a suggestion from
then on, so a family's names stay on its handsets and never go into a published
file.

The comments in the source are long on purpose. Most of them record why something
is the way it is, usually because the obvious version was tried on real hardware
and failed.
