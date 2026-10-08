/* T9 predictive text, ported from the CYD handset (espnow-link/src/t9.cpp).
 *
 * Same dictionary file, same format, same lookup: 52,419 words built by
 * espnow-link/tools/build_t9_dict.py. The CYD proved the design on hardware -
 * "4663" -> good, home, gone - so this is a port, not a redesign.
 *
 * 🔑 THE DIFFERENCE FROM THE CYD: the dictionary is NOT copied into RAM here.
 * It lives in its own flash partition and is memory-mapped, so the words cost
 * ZERO bytes of PSRAM and nothing has to be read at boot. The CYD reads its
 * copy out of LittleFS into PSRAM because the Arduino filesystem API gives it
 * no other option; IDF does.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Maps the dictionary partition. False if it is missing or unreadable - the
 * keypad must degrade to plain digits rather than pretend, so check t9_error().
 * No dictionary means no prediction, NOT a dead keypad. */
bool        t9_begin(void);
const char *t9_error(void);
uint32_t    t9_word_count(void);

/* Candidates for a digit string ("4663"). Exact-length matches come first, in
 * frequency order, then longer words that START with those digits. Returns how
 * many pointers were written into out[]. */
int t9_lookup(const char *digits, const char **out, int max_out);
