/* T9 predictive text, ported from an earlier prototype handset.
 *
 * Same format and lookup as the CYD; ~52,200 words, FILTERED for a child's
 * device, built by tools/build_t9_dict.py. The CYD proved the design on hardware -
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

/* Candidates for a digit string ("4663"): personal words whose keys match
 * exactly, then dictionary words (exact length first, in frequency order, then
 * longer completions), then personal words that are longer completions.
 * Returns how many pointers were written into out[]. */
int t9_lookup(const char *digits, const char **out, int max_out);

/* Personal words (names, nicknames, anything the dictionary lacks), kept per
 * handset in NVS with their capitals. Added by spell mode. False if the word
 * has fewer than two letters. Up to 32; the oldest is replaced when full. */
bool t9_user_add(const char *word);
int  t9_user_count(void);
void t9_user_clear(void);
