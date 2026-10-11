# kids-communicator

A handheld communicator for kids. Two or more of them talk to each other
directly over the air — **no wifi, no internet, no account, no server, and no
subscription.** Turn two on anywhere and they find each other.

**Build one:** [🛒 Parts list](docs/HARDWARE.md) · [🖨️ Case](case/README.md) ·
[💾 Firmware](firmware/README.md) · [🏠 Home Assistant gateway (optional)](docs/GATEWAY.md)

## See it working

Two handsets, recorded straight from their own screens: first-time setup,
pairing, music, Connect 4 played between the two, the talkie, and Solitaire.

<table>
  <tr>
    <td width="50%"><video src="https://github.com/user-attachments/assets/b2dabb0a-016c-4b05-8e15-62bc8102ddb3" controls muted width="100%"></video></td>
    <td width="50%"><video src="https://github.com/user-attachments/assets/813134ef-224e-45d3-b05b-8c8cf97b7fe8" controls muted width="100%"></video></td>
  </tr>
</table>

*Comms (asking questions out loud) at the end of the Kid 1 video uses the
optional [home gateway](docs/GATEWAY.md).*

Built by a dad for his four kids, who have been carrying them daily since
August 2026.

<p align="center">
  <img src="images/home-screen.jpg" width="240" alt="The home screen">
  <img src="images/games.jpg" width="240" alt="The games menu">
  <img src="images/talkie.jpg" width="240" alt="The walkie-talkie">
</p>

## What it does

- **Text messages** between handsets, typed on a T9 keypad with predictive text
  (filtered for children) and a spell mode for names
- **Walkie-talkie**: live voice, push to talk, to one handset or to everyone
- **Games played handset to handset**: tic-tac-toe, Connect 4, checkers,
  Battleship, Hearts, Tanks (artillery, with wind) and Uno for up to four
- **Games for one**: Solitaire, Flamingo (a tap-to-fly side scroller),
  Brick Breaker and Snake. Several of the two-player games, Tanks among them,
  also have a computer opponent
- **Music player**: MP3s from a microSD card, with album art, and the card
  mounts as a USB drive so nobody has to open the case to load music
- **Map** *(experimental)*: an offline vector road map with a GPS position.
  It works, but it is not finished; expect rough edges
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

The cost is honest: no internet radio and no over-the-air updates (the clock
comes from GPS, when the handset has one). For a device handed to a child, that
trade is worth it.

### Home Assistant, without putting your password in a child's pocket

If you run [Home Assistant](https://www.home-assistant.io/), an optional
**gateway** adds **Comms**: press, ask a question out loud, read the answer on
screen. The gateway is a cheap ESP32 that stays at home, plugged in. It holds
the WiFi password and the Home Assistant key, so the handsets still hold none.
A handset that leaves the house just stops being able to ask. See
[`docs/GATEWAY.md`](docs/GATEWAY.md); the ESPHome config is in
[`gateway/`](gateway/).

> **Status: buildable.** The firmware in `firmware/` is the same code that runs
> on the family's handsets, minus the WiFi and smart-home parts. Start with
> [`firmware/README.md`](firmware/README.md).

## Build one

You need the parts below, a 3D printer and a USB cable. Several parts come in
multi-packs, so a second handset costs much less than the first.

| Part | |
|---|---|
| **ESP32-S3 board with a 2.8" touchscreen** (Hosyond / ES3C28P) | Does almost everything: screen, touch, mic, audio, microSD |
| Speaker, LiPo battery, microSD card | |
| 4 × M3 × 6 mm screws | |
| GPS module *(optional)* | For the map and the clock |
| Headphone board *(optional)* | For wired headphones |

👉 **[The full parts list](docs/HARDWARE.md)** has a photo of each part, a
link to where we bought it, and what to match if that link has gone.

Then:

1. **Print the case**: three parts, see [`case/README.md`](case/README.md).
2. **Flash the firmware**: see [`firmware/README.md`](firmware/README.md).
3. **Set up the family**: on each handset, *Settings → Fleet* takes the same
   8-digit fleet code, and *Settings → Radio channel* must match on all of
   them. Then pair them from the Fleet screen.

   📝 **Write the fleet code down and keep it somewhere safe.** You choose it,
   and the handsets never store it: they keep only the encryption key made from
   it, so a lost handset cannot give it away. That also means it **cannot be
   read back** from any handset. You will need it again to add a handset or a
   gateway later. If it is lost, the only way out is a new code on every
   handset and pairing them all again.
4. *(Optional)* **Add a Home Assistant gateway** for Comms: see
   [`docs/GATEWAY.md`](docs/GATEWAY.md).

## The case

Three printed parts, all original work, in `case/`, plus a newer revision of
the whole case as one combined mesh. See `case/README.md` — and
read the note there before regenerating the STLs from the FreeCAD source.

## Status

| Piece | State |
|---|---|
| Firmware | ✅ Builds on ESP-IDF 5.3; tested on real handsets |
| Radio encryption | ✅ Per-link encryption between paired handsets (see below) |
| Case STLs | ✅ Three printable parts, plus the 2026-10-06 revision as one combined mesh |
| Case FreeCAD source | ✅ Included, vendor board model removed |
| Map | 🧪 Experimental |
| Predictive text | ✅ Filtered for children; names are spelled once and remembered per handset |
| Home Assistant gateway | 🧪 Experimental: works, not yet reliable enough for a child alone. See `docs/GATEWAY.md` |
| Parts list | ✅ Every part with a photo and a link |
| Licence | ✅ GPL-3.0, see `LICENSE`, with third-party credits in `NOTICE.md` |

### Radio security: what is protected and what is not

Every handset in a family is set up with the same **fleet code**, typed in once.
Handsets then pair with each other, and each pair gets its own encryption key
derived from that code. The code never leaves the handset.

**Encrypted, between paired handsets:** text messages, game moves, and
walkie-talkie voice, whether to one handset or to everyone.

A message that claims to come from a family handset is accepted only if it
arrives encrypted from a handset that is already paired. A stranger's ESP32
cannot inject a text that looks like it came from a sibling. That was tested by
building exactly that attack and confirming the handsets now drop it.

**Not encrypted:**

- **The presence beacon.** Each handset announces itself about once a second so
  the others know who is in range, and that announcement carries the handset's
  **display name** in the clear. Anyone nearby with an ESP32 can see it. Pick
  names with that in mind.
- **Walkie-talkie "to everyone" with more than four other handsets.** Past four
  listeners the handset switches to a single broadcast, and ESP-NOW cannot
  encrypt broadcasts.

## Credits

Built on [ESP-IDF](https://github.com/espressif/esp-idf),
[LVGL](https://lvgl.io/) and [minimp3](https://github.com/lieff/minimp3).
Full attribution, with each project's licence, is in [`NOTICE.md`](NOTICE.md).
