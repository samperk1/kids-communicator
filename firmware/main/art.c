/*
 * Album art: find the APIC frame in an MP3, decode the JPEG, hand LVGL a
 * bitmap. See art.h for why this did not exist until now.
 *
 * 🚨 EVERYTHING BIG LIVES IN PSRAM. Internal RAM on this board sits on the
 * ~44KB WiFi threshold whenever the audio path is open, and a cover at screen
 * size is 150KB+. The only internal allocation here is tjpgd's 4KB workspace.
 *
 * 🔑 THE DECODER STREAMS FROM THE FILE. tjpgd pulls bytes through a callback,
 * so the JPEG is never held in RAM in one piece — only the decoded bitmap is.
 */

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "rom/tjpgd.h"
#include "hw.h"
#include "art.h"
#include "applog.h"

static const char *TAG = "art";

/* tjpgd's workspace. 3.1KB is the documented requirement; 4 is the round number
 * above it and it is freed the moment the decode finishes. */
#define WORK_SZ     4096
/* A cover is normalised to <=600px by the card pipeline, so 512KB is roughly
 * four times the worst case that can actually arrive. It is a guard against a
 * hand-copied file with a 3000px cover in it, not a working limit. */
#define MAX_PIXBUF  (512 * 1024)

static lv_img_dsc_t s_img;
static uint8_t     *s_pix;

typedef struct {
    FILE   *f;
    long    start;          /* where the JPEG begins in the file */
    long    len;            /* how long it is */
    long    pos;            /* how much has been handed to the decoder */
    uint8_t *out;           /* RGB565 destination */
    int      w, h;
} src_t;

/* ---- ID3v2: find the picture ------------------------------------------- *
 *
 * ⚠️ FRAME SIZES ARE NOT ENCODED THE SAME WAY IN v2.3 AND v2.4. 2.4 uses
 * syncsafe integers (7 bits per byte); 2.3 uses a plain big-endian 32-bit. Read
 * a 2.3 tag as syncsafe and every frame after the first lands at the wrong
 * offset, so the picture is simply never found. The card pipeline writes v2.3
 * (mutagen `v2_version=3`), which is exactly the case that would break.
 */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static uint32_t syncsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((p[1] & 0x7f) << 14) |
           ((p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

/* Walks the tag and leaves the file positioned at the image bytes. */
static bool find_apic(FILE *f, long *start, long *len)
{
    uint8_t h[10];
    if (fread(h, 1, 10, f) != 10) return false;
    if (memcmp(h, "ID3", 3) != 0) return false;

    int ver = h[3];
    long tag_end = 10 + (long)syncsafe32(h + 6);

    /* An extended header, if present, sits before the first frame. */
    if (h[5] & 0x40) {
        uint8_t e[4];
        if (fread(e, 1, 4, f) != 4) return false;
        long ext = (ver >= 4) ? syncsafe32(e) : (long)be32(e) + 4;
        fseek(f, ext - 4, SEEK_CUR);
    }

    while (ftell(f) + 10 <= tag_end) {
        uint8_t fh[10];
        if (fread(fh, 1, 10, f) != 10) return false;
        if (fh[0] == 0) return false;                    /* padding: done */
        long fsz = (ver >= 4) ? (long)syncsafe32(fh + 4) : (long)be32(fh + 4);
        if (fsz <= 0 || ftell(f) + fsz > tag_end) return false;

        if (memcmp(fh, "APIC", 4) != 0) { fseek(f, fsz, SEEK_CUR); continue; }

        long frame_start = ftell(f), frame_end = frame_start + fsz;
        int enc = fgetc(f);                              /* text encoding */

        /* MIME type, always Latin-1 and null terminated. */
        char mime[32];
        size_t n = 0;
        int c;
        while ((c = fgetc(f)) > 0 && ftell(f) < frame_end)
            if (n < sizeof(mime) - 1) mime[n++] = (char)c;
        mime[n] = 0;

        fgetc(f);                                        /* picture type */

        /* Description. ⚠️ In UTF-16 (encodings 1 and 2) the terminator is TWO
         * zero bytes, not one — stopping at the first would leave the decoder
         * pointed at the second half of a character and the JPEG would never
         * start with FFD8. */
        if (enc == 1 || enc == 2) {
            int a, b;
            do {
                a = fgetc(f); b = fgetc(f);
            } while (!(a == 0 && b == 0) && ftell(f) < frame_end);
        } else {
            while ((c = fgetc(f)) > 0 && ftell(f) < frame_end) { }
        }

        if (strstr(mime, "jpeg") == NULL && strstr(mime, "jpg") == NULL) {
            /* PNG covers exist in the wild; the ROM decoder cannot read them,
             * and saying so beats a blank screen and no explanation. */
            applog("[art] picture is %s, not jpeg - skipped", mime);
            fseek(f, frame_end, SEEK_SET);
            continue;
        }
        *start = ftell(f);
        *len   = frame_end - *start;
        return *len > 4;
    }
    return false;
}

/* ---- tjpgd callbacks ---------------------------------------------------- */

static unsigned int in_cb(JDEC *jd, uint8_t *buf, unsigned int nbyte)
{
    src_t *s = (src_t *)jd->device;
    long left = s->len - s->pos;
    if (nbyte > (unsigned int)left) nbyte = (unsigned int)left;
    if (!buf) { fseek(s->f, nbyte, SEEK_CUR); s->pos += nbyte; return nbyte; }
    size_t got = fread(buf, 1, nbyte, s->f);
    s->pos += got;
    return got;
}

/* 🚨 NEVER HAND-PACK RGB565 FOR LVGL. THIS BUILD STORES IT BYTE-SWAPPED.
 *
 * The first version did the obvious thing — (r&0xf8)<<8 | (g&0xfc)<<3 | b>>3 —
 * and the Beastie Boys came out green and magenta. `CONFIG_LV_COLOR_16_SWAP=y`
 * is set here (it is what an SPI panel wants, and flush_cb is a straight memcpy
 * of the buffer to the display), so LVGL's own pixels are stored with the two
 * bytes the other way round. A hand-packed pixel is therefore backwards, and
 * every colour comes out wrong while the picture's SHAPE stays perfectly
 * correct — which is exactly what it looked like.
 *
 * lv_color_make() emits whatever this build of LVGL actually uses, swap or no
 * swap, 16-bit or 32. Let it do the packing and the question cannot come back.
 *
 * The ROM's tjpgd is built with JD_FORMAT 0, so it hands back 3 bytes per pixel
 * and that is not negotiable at runtime. Converting here means the 3-byte form
 * never exists as a whole image — only ever one MCU block of it. */
/* ⚠️ RETURNS `UINT`, NOT `int`. The ROM decoder's prototype is
 * `UINT(*)(JDEC *, void *, JRECT *)` (esp_rom/.../tjpgd.h). Declaring this
 * `int` is a genuine type mismatch that GCC 13 on IDF 5.3 lets past with a
 * warning and GCC 14 on IDF 6.0 rejects outright, since
 * -Wincompatible-pointer-types became an error by default. Not an IDF API
 * change — a latent bug in this file that a newer compiler simply stopped
 * tolerating. */
static UINT out_cb(JDEC *jd, void *bitmap, JRECT *r)
{
    src_t *s = (src_t *)jd->device;
    const uint8_t *p = (const uint8_t *)bitmap;
    for (int y = r->top; y <= r->bottom; y++) {
        if (y >= s->h) { p += (long)(r->right - r->left + 1) * 3; continue; }
        lv_color_t *dst = (lv_color_t *)(s->out +
                          ((long)y * s->w + r->left) * sizeof(lv_color_t));
        for (int x = r->left; x <= r->right; x++, p += 3) {
            if (x >= s->w) continue;
            *dst++ = lv_color_make(p[0], p[1], p[2]);
        }
    }
    return 1;
}

void art_clear(void)
{
    if (s_pix) { heap_caps_free(s_pix); s_pix = NULL; }
    memset(&s_img, 0, sizeof(s_img));
}

bool art_load(const char *path)
{
    art_clear();

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    src_t s = { .f = f };
    if (!find_apic(f, &s.start, &s.len)) { fclose(f); return false; }
    fseek(f, s.start, SEEK_SET);

    uint8_t *work = heap_caps_malloc(WORK_SZ, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!work) { fclose(f); return false; }

    JDEC jd;
    if (jd_prepare(&jd, in_cb, work, WORK_SZ, &s) != JDR_OK) {
        heap_caps_free(work); fclose(f); return false;
    }

    /* 🔑 PICK THE SMALLEST SCALE THAT STILL COVERS THE SCREEN. tjpgd halves at
     * each step, and decoding a 600px cover at full size to show it on a 240px
     * panel would cost four times the memory for pixels nobody can see. */
    int scale = 0;
    while (scale < 3 && (jd.width >> (scale + 1)) >= LCD_W &&
                        (jd.height >> (scale + 1)) >= LCD_W)
        scale++;
    s.w = jd.width  >> scale;
    s.h = jd.height >> scale;

    size_t need = (size_t)s.w * s.h * sizeof(lv_color_t);
    if (need == 0 || need > MAX_PIXBUF) {
        applog("[art] %dx%d needs %u bytes - refused", s.w, s.h, (unsigned)need);
        heap_caps_free(work); fclose(f); return false;
    }
    s.out = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.out) {
        applog("[art] no PSRAM for %u bytes", (unsigned)need);
        heap_caps_free(work); fclose(f); return false;
    }

    JRESULT r = jd_decomp(&jd, out_cb, (uint8_t)scale);
    heap_caps_free(work);
    fclose(f);
    if (r != JDR_OK) {
        heap_caps_free(s.out);
        applog("[art] decode failed: %d", r);
        return false;
    }

    s_pix = s.out;
    s_img.header.cf   = LV_IMG_CF_TRUE_COLOR;
    s_img.header.w    = s.w;
    s_img.header.h    = s.h;
    s_img.data_size   = need;
    s_img.data        = s_pix;
    ESP_LOGI(TAG, "cover %dx%d (source %ux%u, scale 1/%d, %u KB PSRAM)",
             s.w, s.h, jd.width, jd.height, 1 << scale, (unsigned)(need / 1024));
    return true;
}

const lv_img_dsc_t *art_img(void) { return s_pix ? &s_img : NULL; }
