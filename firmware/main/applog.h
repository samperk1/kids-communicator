#pragma once

/* An on-screen log.
 *
 * Unit #1 had one and it was the difference between "it doesn't work" and a
 * diagnosis, because the device is used away from a serial cable. Voice is the
 * reason it matters here: every failure in that chain (no WiFi, 404 STT path,
 * empty transcript, TTS engine wrong) looks identical from the outside — the
 * kid presses talk and nothing happens.
 *
 * ⚠️ Lines are sized to the panel, not to the message. Anything longer is
 * wrapped across lines rather than cut, so a long assistant reply stays
 * readable instead of ending at the first column boundary. */

#define APPLOG_COLS  34
#define APPLOG_LINES 64

void applog_init(void);
void applog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

int  applog_count(void);
/* 0 = oldest still held. Returns NULL past the end. */
const char *applog_line(int i);
/* Bumped on every write, so the UI can repaint only when there is something
 * new rather than rebuilding 64 labels twice a second. */
unsigned applog_seq(void);
