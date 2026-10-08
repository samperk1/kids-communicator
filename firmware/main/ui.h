#pragma once

void ui_init(void);
void ui_show_list(void);
void ui_show_player(void);
/* The log/terminal panel. Reached by long-pressing the status-bar title —
 * deliberately not a visible button; see the note in ui.c. */
void ui_show_term(void);
/* Hold-to-talk. Reached from the Talk button on the now-playing screen. */
void ui_show_talk(void);
/* Walkie-talkie. Opens the channel on entry and closes it on Back — the radio
 * staying up is the biggest battery cost in the whole firmware. */
void ui_show_wt(void);
/* 4-point touch calibration. Long-press the battery side of the status bar. */
void ui_show_cal(void);
/* Ask first. Both entry points are hair triggers - see the note in ui.c. */
void ui_ask_cal(void);
/* The 2x2 home screen: MUSIC / COMMS / RADIO / TALKIE, plus the hamburger.
 * This is where the device boots and where every Back goes. */
void ui_show_home(void);
/* Power, screen timeout and the repair tools, behind the hamburger - kept off
 * the home grid so it does not steal a tile from something kids use. */
void ui_show_menu(void);
void ui_show_fleet(void);
void ui_show_t9(void);
void ui_show_inbox(void);
/* Internet radio. NOTE: "Radio" means internet radio; the walkie-talkie is
 * "Talkie" and the AI assistant is "Comms". The old UI called the walkie-talkie
 * "Radio", which is the collision this naming resolves. */
void ui_show_radio(void);

/* The GPS sky view. Starts the receiver on entry and stops it on exit — it
 * draws 25-50mA while tracking, which is backlight-class. */
void ui_show_gps(void);
void ui_show_map(void);
/* The dongle has no screen: this one chooses its headphones. */
