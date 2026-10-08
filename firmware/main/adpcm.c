#include "adpcm.h"

/* The two IMA ADPCM tables, verbatim from the IMA/DVI specification. Every
 * implementation on earth carries these same numbers; they are not tunable and
 * must not be "improved" — the CYD's Arduino port will carry the identical
 * pair or the two ends will not agree on a single sample. */
static const int16_t STEP[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t INDEX[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static inline int clamp_index(int i)
{
    if (i < 0)  return 0;
    if (i > 88) return 88;
    return i;
}

static inline int16_t clamp_sample(int v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* One sample in, one nibble out. Split out because the encoder and the decoder
 * must apply the SAME reconstruction arithmetic — the encoder predicts from
 * what the decoder will produce, not from the input, or the two drift apart
 * within a few hundred samples. */
static inline uint8_t enc_one(adpcm_state_t *st, int16_t sample)
{
    int step = STEP[st->index];
    int diff = sample - st->predictor;
    uint8_t code = 0;

    if (diff < 0) { code = 8; diff = -diff; }

    /* Three successive halvings of the step: the classic 3-bit magnitude
     * search, unrolled because it is exactly three steps and a loop here would
     * cost more than it saves. */
    int vpdiff = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 1;              vpdiff += step; }

    st->predictor = clamp_sample(code & 8 ? st->predictor - vpdiff
                                          : st->predictor + vpdiff);
    st->index     = (uint8_t)clamp_index(st->index + INDEX[code]);
    return code;
}

static inline int16_t dec_one(adpcm_state_t *st, uint8_t code)
{
    int step   = STEP[st->index];
    int vpdiff = step >> 3;

    if (code & 4) vpdiff += step;
    if (code & 2) vpdiff += step >> 1;
    if (code & 1) vpdiff += step >> 2;

    st->predictor = clamp_sample(code & 8 ? st->predictor - vpdiff
                                          : st->predictor + vpdiff);
    st->index     = (uint8_t)clamp_index(st->index + INDEX[code]);
    return st->predictor;
}

void adpcm_encode(adpcm_state_t *st, const int16_t *in, uint8_t *out, int n)
{
    st->index = (uint8_t)clamp_index(st->index);
    for (int i = 0; i < n; i += 2) {
        uint8_t lo = enc_one(st, in[i]);
        uint8_t hi = enc_one(st, in[i + 1]);
        out[i >> 1] = (uint8_t)(lo | (hi << 4));   /* low nibble first */
    }
}

void adpcm_decode(adpcm_state_t *st, const uint8_t *in, int16_t *out, int n)
{
    /* ⚠️ CLAMP WHAT CAME OFF THE AIR. `index` arrives in a frame header from
     * another device; a corrupted byte would index STEP[] out of bounds and
     * read whatever follows it in flash. Cheap insurance on a value we do not
     * control. */
    st->index = (uint8_t)clamp_index(st->index);
    for (int i = 0; i < n; i += 2) {
        uint8_t b = in[i >> 1];
        out[i]     = dec_one(st, (uint8_t)(b & 0x0F));
        out[i + 1] = dec_one(st, (uint8_t)(b >> 4));
    }
}
