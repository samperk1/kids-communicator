#pragma once
#include <stdbool.h>

/* Text messaging against Home Assistant.
 *
 * The inbox is an ordinary `input_text` helper — the builder already had this one. It is
 * capped at 100 characters by HA, which is the message length limit for now. */
#define MSG_INBOX_ENTITY  "input_text.kids_message"

void msg_init(void);

/* This handset in words - its Settings name, or its comm_XXXX id if unnamed. */
const char *msg_dev_name(void);

/* A comm_XXXX id in words. With no name table in this build, returns the id. */
const char *msg_name_for(const char *id);

/* Poll for a new message. Call only with the radio already up; returns true if
 * something new arrived. */
bool msg_poll(void);

/* Check the OTHER handsets' message entities. Same cycle, one more GET, no
 * extra radio time. Returns true if something new arrived. */
bool msg_poll_peers(void);

/* Send a message to the thread. Blocks on the network — call it from a task of
 * its own, never from the UI thread. */
bool msg_send(const char *text);

/* --- the inbox ------------------------------------------------------------
 *
 * 🚨 THIS BOARD HAD NO MESSAGE STORE AT ALL. msg_text() is the LATEST message
 * and nothing else: a second one arriving overwrote the first, and a popup
 * dismissed was a message gone. The CYD handset has kept an NVS inbox since it
 * was built, and the builder asked for the same here.
 *
 * Newest first, capped, and persisted - a message that arrives while the
 * handset is in a pocket must still be there when it comes out. */
#define MSG_INBOX_MAX  10

uint8_t     msg_inbox_count(void);
const char *msg_inbox_text(uint8_t i);      /* 0 = newest */
void        msg_inbox_clear(void);

bool        msg_have_new(void);
const char *msg_text(void);
void        msg_mark_read(void);
/* How many messages arrived since the handset last looked — more than one means
 * it was asleep or out of range while they piled up. */
int         msg_missed(void);
