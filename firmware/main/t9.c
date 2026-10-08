/* T9 predictive text — see t9.h. Ported from the CYD's t9.cpp.
 *
 * 🔑 THE DIGIT KEY IS NOT STORED. It is derivable from the letters, so storing
 * it would spend a byte per record repeating what the word already says.
 *
 * 🔑 RECORDS ARE SORTED BY DIGIT KEY, so every word matching a typed prefix is
 * a CONTIGUOUS RANGE. Lookup is a binary search for the start of that range,
 * not a scan of 52,419 words on every keypress.
 *
 * And because "466" sorts before "4663", walking forward from the start of the
 * range yields the exact-length matches FIRST — in frequency order, because
 * rank is the secondary sort — and only then the longer completions. That is
 * exactly the order a T9 wants to offer them in, for free.
 *
 * File layout: "T9D1", u32 record count, then records of
 * [len:1][rank:1][word:len], not NUL-terminated.
 */

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_partition.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "applog.h"
#include "t9.h"

static const uint8_t *dict;          /* mmap'd, straight out of flash */
static esp_partition_mmap_handle_t map_handle;
static uint32_t *offs;               /* record offsets, PSRAM */
static uint32_t  nrec;
static char      errbuf[96] = "not loaded";

/* letter -> keypad digit */
static const char LETTER_DIGIT[26] = {
    '2','2','2','3','3','3','4','4','4','5','5','5','6',
    '6','6','7','7','7','7','8','8','8','9','9','9','9'
};
static inline char digit_of(char c)
{
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return (c >= 'a' && c <= 'z') ? LETTER_DIGIT[c - 'a'] : '?';
}

/* ------------------------------------------------------------------ *
 * PERSONAL WORDS — the words the dictionary will never have.
 *
 * 🚨 NAMES WERE IMPOSSIBLE TO TYPE. The dictionary builder drops every
 * capitalised word (proper nouns), and multi-tap was removed when prediction
 * came in, so a word outside the dictionary had no way in at all. The builder,
 * 2026-10-08: "some thing that is not working well with prodictive is names".
 *
 * Spell mode (ui.c) types a word letter by letter and hands it here. It is kept
 * on THIS handset, in NVS, with its capitals and apostrophe ("O'Neil"), and
 * offered ahead of the dictionary whenever its keys match. 🔑 The names live
 * per handset and never in the dictionary file, which is published.
 *
 * Fixed slots, replaced oldest-first when full. A typo that got saved is gone
 * after T9_USER_MAX newer words, and re-spelling a word moves nothing.
 * ------------------------------------------------------------------ */
#define T9_USER_MAX  32
#define T9_USER_LEN  16              /* 15 characters + NUL */
static const char *USER_NS = "kct9user";   /* see fleet.c: kept apart from the family build */

static struct {
    uint8_t ver;
    uint8_t next;                    /* slot the next new word goes into */
    char    w[T9_USER_MAX][T9_USER_LEN];
} s_user;
static bool s_user_loaded;

static void user_load(void)
{
    if (s_user_loaded) return;
    s_user_loaded = true;
    memset(&s_user, 0, sizeof(s_user));
    nvs_handle_t h;
    if (nvs_open(USER_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_user);
    if (nvs_get_blob(h, "words", &s_user, &len) != ESP_OK ||
        len != sizeof(s_user) || s_user.ver != 1) {
        memset(&s_user, 0, sizeof(s_user));      /* versioned: wipe, never misread */
    }
    nvs_close(h);
    for (int i = 0; i < T9_USER_MAX; i++) s_user.w[i][T9_USER_LEN - 1] = 0;
    if (s_user.next >= T9_USER_MAX) s_user.next = 0;
}

static void user_save(void)
{
    nvs_handle_t h;
    if (nvs_open(USER_NS, NVS_READWRITE, &h) != ESP_OK) return;
    s_user.ver = 1;
    nvs_set_blob(h, "words", &s_user, sizeof(s_user));
    nvs_commit(h);
    nvs_close(h);
}

/* The digits a word is typed with. Letters only: the apostrophe in "O'Neil"
 * has no key, so the word is reached by 6-6-3-4-5 like any other. */
static int user_key(const char *w, char *out, int n)
{
    int k = 0;
    for (; *w && k < n - 1; w++)
        if (isalpha((unsigned char)*w)) out[k++] = digit_of(*w);
    out[k] = 0;
    return k;
}

bool t9_user_add(const char *word)
{
    user_load();
    char clean[T9_USER_LEN];
    int n = 0, letters = 0;
    for (const char *p = word; *p && n < T9_USER_LEN - 1; p++) {
        char c = *p;
        if (isalpha((unsigned char)c)) { clean[n++] = c; letters++; }
        else if (c == '\'' || c == '-') clean[n++] = c;
    }
    clean[n] = 0;
    if (letters < 2) return false;               /* "a", "I" need no saving */

    for (int i = 0; i < T9_USER_MAX; i++) {
        if (s_user.w[i][0] && !strcasecmp(s_user.w[i], clean)) {
            if (strcmp(s_user.w[i], clean)) {    /* same word, newer capitals */
                strlcpy(s_user.w[i], clean, T9_USER_LEN);
                user_save();
            }
            return true;
        }
    }
    strlcpy(s_user.w[s_user.next], clean, T9_USER_LEN);
    s_user.next = (uint8_t)((s_user.next + 1) % T9_USER_MAX);
    user_save();
    applog("[t9] saved personal word \"%s\"", clean);
    return true;
}

int t9_user_count(void)
{
    user_load();
    int n = 0;
    for (int i = 0; i < T9_USER_MAX; i++) if (s_user.w[i][0]) n++;
    return n;
}

void t9_user_clear(void)
{
    user_load();
    memset(&s_user, 0, sizeof(s_user));
    user_save();
    applog("[t9] personal words cleared");
}

/* Personal words whose key matches the typed digits. exact=true: the key IS
 * the digits; false: the key is longer and starts with them. */
static int user_matches(const char *digits, int dlen, bool exact,
                        const char **out, int max_out)
{
    int n = 0;
    for (int i = 0; i < T9_USER_MAX && n < max_out; i++) {
        if (!s_user.w[i][0]) continue;
        char k[T9_USER_LEN];
        int klen = user_key(s_user.w[i], k, sizeof(k));
        if (klen < dlen || strncmp(k, digits, dlen)) continue;
        if (exact != (klen == dlen)) continue;
        out[n++] = s_user.w[i];
    }
    return n;
}

const char *t9_error(void)      { return errbuf; }
uint32_t    t9_word_count(void) { return nrec; }

/* Compare record i's key against the typed digits, PREFIX-wise:
 * <0 record sorts before, 0 record starts with digits, >0 after. */
static int cmp_prefix(uint32_t i, const char *digits, int dlen)
{
    const uint8_t *r = dict + offs[i];
    int wlen = r[0];
    const char *w = (const char *)(r + 2);
    for (int k = 0; k < dlen; k++) {
        if (k >= wlen) return -1;              /* record shorter => sorts first */
        char d = digit_of(w[k]);
        if (d < digits[k]) return -1;
        if (d > digits[k]) return 1;
    }
    return 0;
}

bool t9_begin(void)
{
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "t9dic");
    if (!p) {
        snprintf(errbuf, sizeof(errbuf), "no 't9dic' partition in the table");
        return false;
    }

    /* 🔑 MAPPED, NOT READ. The words are addressed directly in flash: no PSRAM
     * copy, no multi-hundred-KB read at boot, and the pages the keypad never
     * touches are never fetched. ⚠️ The mapping must outlive every pointer
     * t9_lookup() hands out, so the handle is kept for the life of the app and
     * deliberately never unmapped. */
    const void *base = NULL;
    esp_err_t e = esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA,
                                     &base, &map_handle);
    if (e != ESP_OK) {
        snprintf(errbuf, sizeof(errbuf), "mmap failed: %s", esp_err_to_name(e));
        return false;
    }
    dict = (const uint8_t *)base;

    if (memcmp(dict, "T9D1", 4) != 0) {
        /* An unwritten partition is 0xFF everywhere, which is exactly what a
         * board that was flashed but never had the dictionary uploaded looks
         * like. Say which it is, or it reads as corruption. */
        snprintf(errbuf, sizeof(errbuf),
                 dict[0] == 0xFF ? "t9dic partition is empty - upload t9.dic"
                                 : "bad magic in t9dic");
        dict = NULL;
        return false;
    }
    memcpy(&nrec, dict + 4, 4);
    if (!nrec || nrec > 200000) {
        snprintf(errbuf, sizeof(errbuf), "implausible record count %u",
                 (unsigned)nrec);
        nrec = 0; dict = NULL;
        return false;
    }

    /* The index is the only thing that costs RAM: 4 bytes per word, in PSRAM,
     * which this board has 7.5MB of. */
    offs = heap_caps_malloc((size_t)nrec * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!offs) {
        snprintf(errbuf, sizeof(errbuf), "no PSRAM for a %u-entry index",
                 (unsigned)nrec);
        nrec = 0; dict = NULL;
        return false;
    }

    uint32_t pos = 8;
    for (uint32_t i = 0; i < nrec; i++) {
        if (pos + 2 > p->size) {
            snprintf(errbuf, sizeof(errbuf), "index overran at %u",
                     (unsigned)i);
            nrec = i;
            break;
        }
        offs[i] = pos;
        pos += 2 + dict[pos];
    }
    snprintf(errbuf, sizeof(errbuf), "ok");
    applog("[t9] %u words mapped from flash, index %uKB in PSRAM",
           (unsigned)nrec, (unsigned)(nrec * 4 / 1024));
    return nrec > 0;
}

/* Dictionary candidates only, in file order (exact-length first). */
static int dict_lookup(const char *digits, int dlen, const char **out, int max_out)
{
    if (!nrec) return 0;

    /* Lower bound: first record that does not sort before the typed digits. */
    uint32_t lo = 0, hi = nrec;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (cmp_prefix(mid, digits, dlen) < 0) lo = mid + 1;
        else                                   hi = mid;
    }

    /* Words are not NUL-terminated in the file, so each candidate is copied out
     * into a small static pool. */
    static char pool[12][12];
    int n = 0;
    for (uint32_t i = lo; i < nrec && n < max_out && n < 12; i++) {
        if (cmp_prefix(i, digits, dlen) != 0) break;
        const uint8_t *r = dict + offs[i];
        int wlen = r[0];
        if (wlen > 11) continue;
        memcpy(pool[n], r + 2, wlen);
        pool[n][wlen] = 0;
        out[n] = pool[n];
        n++;
    }
    return n;
}

/* 🔑 PERSONAL WORDS FIRST. A word someone took the trouble to spell out is the
 * one they meant: "Kim" ahead of every dictionary word on 5-4-6. Then the dictionary
 * (exact before longer), then personal words that are longer completions. A
 * dictionary word that matches a personal one, ignoring case, is dropped so
 * "Bob" and "bob" do not both appear. Works with no dictionary loaded. */
int t9_lookup(const char *digits, const char **out, int max_out)
{
    if (!digits || !*digits || max_out <= 0) return 0;
    user_load();
    int dlen = (int)strlen(digits);

    int n = user_matches(digits, dlen, true, out, max_out);
    int nu = n;

    const char *d[12];
    int nd = dict_lookup(digits, dlen, d, 12);
    for (int i = 0; i < nd && n < max_out; i++) {
        bool dup = false;
        for (int j = 0; j < nu; j++) if (!strcasecmp(out[j], d[i])) dup = true;
        if (!dup) out[n++] = d[i];
    }
    if (n < max_out) n += user_matches(digits, dlen, false, out + n, max_out - n);
    return n;
}
