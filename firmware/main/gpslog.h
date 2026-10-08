#pragma once
#include <stdbool.h>

/* The GPS session logger — what turns walking around with a handset into a
 * measurement.
 *
 * 🔑 WHY THIS EXISTS AT ALL. Comparing two antenna placements by reading
 * numbers off a screen, outdoors, while holding the handset at arm's length in
 * the position being tested, does not work: the numbers move every second and
 * the act of looking changes how you are holding it. So the handset records
 * them, and the comparison happens later at a desk.
 *
 * The procedure it supports:
 *   hold placement A -> tap MARK -> wait for the window to close ->
 *   move to B -> tap MARK -> ... -> read the summary rows off the card.
 *
 * ⚠️ A 30-second window, not an instant reading. C/N0 on a single satellite
 * wanders by several dB on its own; an instant comparison measures that wander
 * rather than the placement. */

#define GPSLOG_WINDOW_MS  30000

/* Opens a numbered session file under /sdcard/gpslog/ and writes the header.
 * Safe to call when the card is absent — it simply does nothing, and the GPS
 * screen keeps working without a log. */
bool gpslog_start(void);
void gpslog_stop(void);
bool gpslog_active(void);

/* Call at 1 Hz with the screen's refresh. Appends one row and, if a MARK window
 * is open, accumulates into it. */
void gpslog_tick(void);

/* Begin a labelled 30-second averaging window. Labels run A, B, C… so the
 * button needs no keyboard. Returns the label it just started. */
char gpslog_mark(void);

/* For the UI: seconds left in the open window, 0 when none is running. */
int  gpslog_window_left(void);
/* The result of the last completed window — empty string until there is one.
 * Never NULL. */
const char *gpslog_last_result(void);

/* The session file's name, for showing on screen so the right file can be
 * found on the card afterwards. Never NULL. */
const char *gpslog_name(void);

/* Display state stamped into each row, so the display-desense A/B can be
 * separated from the rest of the walk when the log is read back. */
void gpslog_set_display_state(const char *s);

/* Print a session to the console: MARK summaries first, then every row.
 * session <= 0 means the newest. See the note in gpslog.c for why the
 * summaries come first. */
void gpslog_dump(int session);

/* ---- the RAM sink ---------------------------------------------------------
 *
 * 🔑 THE CARD A/B CANNOT LOG TO THE CARD. Its quiet half UNMOUNTS the card —
 * that is the whole point, an SDMMC peripheral with no clock running — so for
 * the duration of that test there is nowhere to write. Rows go to a PSRAM
 * buffer instead and the whole session is written out in one go at the end,
 * once the card is back.
 *
 * ⚠️ The buffer is finite and the run is timed, so it can fill. When it does,
 * rows are DROPPED and the count is reported — a truncated log that says it was
 * truncated, never a silent short file. */
bool   gpslog_ram_begin(size_t cap);   /* start buffering; card not required */
bool   gpslog_ram_commit(void);        /* write the buffer to a session file */
void   gpslog_ram_abort(void);         /* throw it away, free the buffer */
bool   gpslog_ram_active(void);
size_t gpslog_ram_dropped(void);       /* rows lost to a full buffer */
