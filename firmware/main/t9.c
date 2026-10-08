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
#include "esp_partition.h"
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
    return (c >= 'a' && c <= 'z') ? LETTER_DIGIT[c - 'a'] : '?';
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

int t9_lookup(const char *digits, const char **out, int max_out)
{
    if (!nrec || !digits || !*digits) return 0;
    int dlen = (int)strlen(digits);

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
