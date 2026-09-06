# kids-communicator

A handheld communicator for kids. Two or more of them talk to each other
directly over the air — **no wifi, no internet, no account, no server, and no
subscription.** Turn two on anywhere and they find each other.

Built by a dad for his four kids, who have been carrying them daily since
August 2026.

## What it does

- **Text messages** between handsets, typed on a T9 keypad
- **Walkie-talkie** — live voice, push to talk, one-to-one or to everyone
- **Eight two-player games**, played handset to handset: tic-tac-toe, Connect 4,
  Hearts, Battleship, Solitaire, Uno, Dots and Boxes and checkers
- **Music player** — MP3s from a microSD card, with album art
- Runs on a battery, in a 3D-printed case, in a pocket

## Why there is no wifi

This is the central design decision, and everything follows from it.

The handsets talk over **ESP-NOW** — Espressif's direct radio protocol. There is
no access point to join, so:

- **the firmware holds no credentials of any kind.** No wifi password, no API
  token, nothing to leak and nothing to type in.
- **it opens no network sockets.** A handset cannot be reached from the internet
  because it is not on the internet.
- **it works anywhere** — a campsite, a car, a school playground. Nothing to set
  up when you get there.

The cost is honest: no internet radio, no clock sync, no over-the-air updates,
and no smart-home integration. For a device handed to a child, that trade is
worth it.

> **Status: not ready to build yet.** The case files are final and the firmware
> works — it is running on four handsets today — but the public firmware here is
> still being separated from the family version it came from. See *Status* below.

## Hardware

One board does almost everything: an **ESP32-S3 with a 2.8" touchscreen**
(sold as the ES3C28P, and by Hosyond and LCDwiki among others), plus a speaker,
a microphone, a battery and a microSD card.

Full parts list with pictures and part numbers: `docs/HARDWARE.md` *(in
progress)*.

## The case

Three printed parts, all original work, in `case/`. See `case/README.md` — and
read the note there before regenerating the STLs from the FreeCAD source.

## Status

| Piece | State |
|---|---|
| Case STLs | ✅ Final, exported, verified as solid meshes |
| Case FreeCAD source | ⏳ Held back pending a licensing question about an embedded vendor board model |
| Firmware | ⏳ Being separated from the family build |
| Parts list | ⏳ Photographs being taken |
| Radio encryption | 🚨 **Not done.** ESP-NOW traffic is currently unencrypted — see below |
| Licence | ⏳ Not yet chosen |

### 🚨 Before anyone builds one of these

ESP-NOW traffic is **not encrypted and not authenticated** today. Anyone within
radio range with a cheap ESP32 could listen to the walkie-talkie or inject a
text message that looks like it came from a sibling. That is acceptable for the
family this was built for and **is not acceptable for a device published for
other people's children.** It is the top open item and it will be fixed before
this repo goes public.

## Credits

Built on [ESP-IDF](https://github.com/espressif/esp-idf),
[LVGL](https://lvgl.io/) and [minimp3](https://github.com/lieff/minimp3).
Full attribution once the licence audit is complete.
