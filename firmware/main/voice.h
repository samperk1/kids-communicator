#pragma once
#include <stdbool.h>

/* Hold-to-talk: mic -> HA speech-to-text -> Assist agent -> TTS -> speaker.
 *
 * Split the same way the microphone is, and for the same reason: the whole
 * interaction spans a finger on a button plus ~10s of network, and LVGL event
 * callbacks must not block. The UI only raises flags; voice_service() does the
 * work from the main loop. */

typedef enum {
    VOICE_IDLE = 0,
    VOICE_LISTENING,
    VOICE_THINKING,
    VOICE_SPEAKING,
    VOICE_ERROR,
} voice_stage_t;

void voice_init(void);

/* Called from LVGL press/release handlers. Cheap, non-blocking. */
void voice_press(void);
void voice_release(void);

/* Called from the main loop. Does the recording and the network work. */
void voice_service(void);

voice_stage_t voice_stage(void);
const char   *voice_heard(void);     /* last transcript */
const char   *voice_reply(void);     /* last assistant reply */
/* A specific, actionable hint ("keep holding"), or empty. Beats a generic
 * error for the person actually using this. */
const char   *voice_note(void);

/* Stop waiting in line for a busy gateway. Cheap; safe from an LVGL callback,
 * and a no-op when nothing is waiting. */
void          voice_cancel_wait(void);
