# The Home Assistant gateway (optional)

The handsets have no WiFi. The gateway is how they reach Home Assistant
anyway: a cheap ESP32 that stays at home, listens to the handsets over the same
radio they use to talk to each other, and passes questions to Home Assistant's
voice assistant. The answer comes back to the handset as text on its screen.

**You do not need one.** Texting, the walkie-talkie, games and music all work
without it. A gateway adds **Comms**: press, ask a question out loud, read the
answer.

## Why it is a separate board

Home Assistant access tokens cannot be limited to a few devices. A token
carries the full permissions of whoever made it. If the handsets talked to
Home Assistant directly, every one would carry your WiFi password and a key to
your whole house, in flash, on a device a child takes to school with the USB
port on the outside of the case.

So the credentials live on a board that never leaves the house:

| | Handset | Gateway |
|---|---|---|
| WiFi password | none | yes |
| Home Assistant key | none | yes (ESPHome's own API key, no access token) |
| Fleet code | yes | yes |
| Leaves the house | yes | no |

A handset that leaves the house simply stops being able to ask, so there is
nothing to revoke and no list to keep up to date. Having the gateway in your
house is the permission.

## What you need

- **Any original ESP32 board** with 4 MB of flash: an "ESP32 DevKit" or
  "ESP32-WROOM-32" board is fine. Not an ESP32-S3, C3 or C6: the config
  assumes the original chip. No PSRAM needed.
- A USB cable and a USB charger to leave it plugged into.
- **Home Assistant** with the **ESPHome Device Builder** add-on, and a voice
  assistant set up under *Settings → Voice assistants*. The gateway uses its
  speech-to-text and conversation agent; it does not bring its own.

## ⚠️ First, the channel

This is the one setup step that silently breaks everything if it is wrong.

A gateway on WiFi has a single radio, and the WiFi router decides which channel
that radio sits on. The handsets must be on **the same channel**, or they
cannot hear the gateway at all, and nothing on the handset says why.

1. Find your router's 2.4 GHz channel in its settings, and **pin it** to that
   channel (20 MHz wide, not "auto"), so it cannot move later. Channels 1, 6
   and 11 are the usual choices.
2. On **every handset**: *Settings → Radio channel*, set to the same number.
   All handsets must match each other too.
3. In your gateway file, set `fleet_channel` to the same number (the default
   is 11).

If the gateway ends up on the wrong channel, Home Assistant shows it with the
error **"ESP-NOW is on the wrong channel"**.

> **An Ethernet ESP32** (WT32-ETH01, Olimex ESP32-POE) would avoid this
> entirely: it reaches Home Assistant over the cable, leaving its radio free
> to sit on any channel. ESPHome supports those boards, but this has **not
> been tested** with the gateway yet.

## Setting it up

You need your family's **fleet code**: the 8 digits you chose and typed into
every handset. The handsets cannot show it to you; they keep only the key made
from it. If nobody wrote it down, choose a new code, enter it on every handset
(*Settings → Fleet*), pair the handsets with each other again, and use the new
code here.

1. **Secrets.** Open `/config/esphome/secrets.yaml` in Home Assistant (the
   ESPHome add-on's *Secrets* button). Add the lines from
   [`gateway/secrets.yaml.example`](../gateway/secrets.yaml.example) with your
   own values. `comm_fleet_code` is the same 8-digit code you typed into the
   handsets.
2. **The gateway file.** Copy
   [`gateway/example-gateway.yaml`](../gateway/example-gateway.yaml) into
   `/config/esphome/`, renamed to match its `device_name`. Set `fleet_channel`
   if you are not on 11. It pulls everything else from this repository, so
   later fixes arrive with a normal ESPHome update.
3. **First install, over USB.** Plug the ESP32 into the computer you are
   browsing from. In ESPHome: *Install → Plug into this computer*. After this,
   updates go over WiFi.
4. **Add it to Home Assistant.** It appears under *Settings → Devices &
   services* as discovered. Click *Configure* and give it the
   `gateway_api_key` from your secrets.
5. **Pair each handset with it.** On the handset: *Settings → Fleet*, tap the
   gateway (it is listed as `GW-` and four characters). Each handset pairs
   once.
6. **Ask something.** Press **Comms** on the handset, speak, let go. It shows
   *Thinking...* and then the answer.

Leave the gateway somewhere central and plugged in. More than one is fine:
give each its own file, name and API key. Handsets use the strongest gateway
that can reach Home Assistant. *Settings → Ask via* shows which one, and can
pin a particular one.

## When it does not work

| What you see | What it usually is |
|---|---|
| The gateway is not in the handset's Fleet list | Wrong channel. See above. |
| Home Assistant: "ESP-NOW is on the wrong channel" | The router moved the gateway. Pin the router's channel, or change the handsets to match. |
| Home Assistant: "Invalid encryption key" | Home Assistant reused an **older device entry** for the same board, with an old key. Delete the device in Home Assistant and add it again. |
| Pairing gets no answer | One end still remembers an old pairing. On the handset, tap the gateway in *Fleet* to **remove** it, restart both, and pair again. |
| "Lost audio" on some questions, answers on others | Look at Home Assistant's voice pipeline debug (*Settings → Voice assistants → ⋯ → Debug*). If the question shows up there, the radio part worked and the speech-to-text step failed. Try another speech-to-text engine. |

🚨 **Erasing the gateway wipes its pairings.** Reinstalling it from scratch,
or moving it to a new board, means every handset must *remove* it from Fleet
and pair again. A handset that still holds the old pairing sends encrypted
messages the gateway can no longer read, and they are dropped without any
error. A normal ESPHome update does **not** erase pairings.

The gateway's log (ESPHome → *Logs*) shows every pairing and every question:
`paired <name>`, `<id> asking: ... bytes`, `heard: ...`, `answer: ...`. That is
the first place to look.

## Limits

- **Answers are text, by design.** The handset shows the answer; the gateway
  does not stream speech back.
- **Questions up to about 12 seconds.**
- The gateway holds the fleet code. Anyone with the board and a USB cable could
  read it out, so treat it like the handsets: it belongs to your house.
