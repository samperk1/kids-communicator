# Parts list

⏳ **In progress** — photographs and exact part numbers are being collected.

## The one part that matters

An **ESP32-S3 development board with a built-in 2.8" touchscreen**, sold under
several names — ES3C28P, and by Hosyond and LCDwiki among others. It carries the
processor, the display, the capacitive touch panel, the audio codec, the
amplifier and the microSD slot on one board.

Notes that will save you money:

- ⚠️ **PSRAM is not optional** for this firmware, and listings are not reliable
  about whether a board has it. Buy from a seller who states it plainly.
- ⚠️ **The ESP32-S3 has no Bluetooth Classic**, only BLE. If you find a guide
  claiming an S3 can pair with ordinary Bluetooth headphones, it is wrong.

## Everything else

| Part | Notes |
|---|---|
| Speaker | ⏳ |
| Microphone | ⏳ |
| Battery | ⏳ Lithium pouch cell — **must have a protection circuit at the tab** |
| microSD card | Any reasonable size; music and map tiles |
| GPS module *(optional)* | ⏳ ATGM336H-class UART module. Only needed for the map and the clock |
| Headphone board *(optional)* | ⏳ PCM5102A I2S DAC with a 3.5 mm jack, for wired headphones |
| Fasteners | ⏳ |

## The Home Assistant gateway *(optional)*

Only needed for Comms. See [`GATEWAY.md`](GATEWAY.md).

| Part | Notes |
|---|---|
| ESP32 board | Any **original** ESP32 with 4 MB flash, e.g. an "ESP32 DevKit" / ESP32-WROOM-32 board. Not an S3, C3 or C6. No PSRAM needed |
| USB cable + charger | It stays plugged in at home |
| *(alternative, untested)* Ethernet ESP32 | WT32-ETH01 or Olimex ESP32-POE: frees the radio from the WiFi router's channel |

## Printed parts

Three, in `../case/stl/`. See `../case/README.md`.
