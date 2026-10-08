/*
 * The music library: mount the card, walk /music/<Artist>/<Album>/<Track>.mp3
 * once at boot, and hold the result in PSRAM.
 *
 * A full walk of the builder's 1,226-track card takes a few seconds, which is why unit
 * #1 caches it. Doing that here too is planned (P2), but the scan comes first:
 * a cache of nothing is not worth writing.
 *
 * ⚠️ Long filenames need CONFIG_FATFS_LFN_HEAP + MAX_LFN=255, set in
 * sdkconfig.defaults. Without them opendir returns mangled 8.3 names and the
 * paths never resolve.
 * ⚠️ format_if_mount_failed stays FALSE. The examples default it to true, which
 * would reformat a 7.3GB library on a bad mount.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "hw.h"
#include "library.h"

static const char *TAG = "lib";

static sdmmc_card_t *s_card;
static lib_artist_t *s_artists;
static lib_album_t  *s_albums;
static lib_track_t  *s_tracks;
static int s_n_artists, s_n_albums, s_n_tracks;
/* Album indices in NAME order — the flat album browse. Rebuilt by
 * build_album_order() on BOTH paths into library_scan(), because a cache hit
 * never runs the walk that produces the arrays. */
static int *s_alb_order;
static int  s_n_alb_order;

/* 🚨 THESE ARE HARD CAPS AND THE WALK STOPS DEAD AT THEM — SILENTLY.
 *
 * readdir() below is guarded by `s_n_artists < MAX_ARTISTS`, so a library
 * bigger than the cap is not truncated with a warning, it simply ends. At 256
 * artists the builder's own 386-artist library lost a third of itself with nothing in
 * the log to say so.
 *
 * Raised 2026-09-05 for the full library: 386 artists, 601 albums, 3,989 MP3s —
 * and note the old MAX_TRACKS of 4096 had only 107 tracks of headroom left, so
 * it was about to become the next silent ceiling.
 *
 * The cost is PSRAM, not internal RAM, which is the scarce thing here:
 *   512 artists x 140B  =  70 KB
 *  2048 albums  x 140B  = 287 KB
 *  8192 tracks  x 132B  = 1.05 MB
 * about 1.4 MB of the 7.25 MB free. */
#define MAX_ARTISTS 512
#define MAX_ALBUMS  2048
#define MAX_TRACKS  8192

int  library_artist_count(void) { return s_n_artists; }
const lib_artist_t *library_artist(int i)
{
    return (i >= 0 && i < s_n_artists) ? &s_artists[i] : NULL;
}

char library_artist_letter(int i)
{
    if (i < 0 || i >= s_n_artists) return '#';
    unsigned char c = (unsigned char)library_sortkey(s_artists[i].name)[0];
    c = (unsigned char)toupper(c);
    return (c >= 'A' && c <= 'Z') ? (char)c : '#';
}

/* The artists are sorted by the same key the letter comes from, so a letter is
 * one contiguous run and the UI can show it by moving the loop bounds - no
 * filtered copy of the list, and no per-row test. */
bool library_letter_range(char letter, int *first, int *count)
{
    int f = -1, n = 0;
    for (int i = 0; i < s_n_artists; i++) {
        if (library_artist_letter(i) != letter) { if (f >= 0) break; continue; }
        if (f < 0) f = i;
        n++;
    }
    if (first) *first = (f < 0) ? 0 : f;
    if (count) *count = n;
    return f >= 0;
}

char library_first_letter(void)
{
    return s_n_artists > 0 ? library_artist_letter(0) : 'A';
}

int library_album_count(int artist)
{
    return (artist >= 0 && artist < s_n_artists) ? s_artists[artist].n_albums : 0;
}

const lib_album_t *library_album(int artist, int n)
{
    if (artist < 0 || artist >= s_n_artists) return NULL;
    if (n < 0 || n >= s_artists[artist].n_albums) return NULL;
    return &s_albums[s_artists[artist].first_album + n];
}

/* ---- shuffle support ------------------------------------------------------
 *
 * 🔑 COUNTS ONLY, NEVER CONTIGUITY. It is tempting to treat an artist's tracks
 * as one contiguous run of the global track array and pick with a single
 * offset — but the albums are SORTED after the walk while `first_track` keeps
 * pointing into the unsorted track array, so an artist's albums are adjacent
 * and their TRACKS are not. Walking album counts is a few hundred iterations at
 * most and is correct whatever the sort did. */
int library_total_tracks(void) { return s_n_tracks; }

int library_artist_track_total(int artist)
{
    if (artist < 0 || artist >= s_n_artists) return 0;
    return s_artists[artist].n_tracks;
}

/* Map a 0-based index over EVERY track on the card to (album, track). */
bool library_pick_global(int g, int *album_out, int *track_out)
{
    if (g < 0) return false;
    for (int i = 0; i < s_n_albums; i++) {
        int n = s_albums[i].n_tracks;
        if (g < n) { *album_out = i; *track_out = g; return true; }
        g -= n;
    }
    return false;
}

/* Map a 0-based index over ONE artist's tracks to (album, track). */
bool library_pick_in_artist(int artist, int g, int *album_out, int *track_out)
{
    if (artist < 0 || artist >= s_n_artists || g < 0) return false;
    const lib_artist_t *ar = &s_artists[artist];
    for (int j = 0; j < ar->n_albums; j++) {
        int idx = ar->first_album + j;
        if (idx < 0 || idx >= s_n_albums) break;
        int n = s_albums[idx].n_tracks;
        if (g < n) { *album_out = idx; *track_out = g; return true; }
        g -= n;
    }
    return false;
}

const lib_album_t *library_album_by_index(int idx)
{
    return (idx >= 0 && idx < s_n_albums) ? &s_albums[idx] : NULL;
}

static int letter_bucket(const char *key);   /* defined with the artist sort */

/* ---- the flat album list -------------------------------------------------
 * s_albums is grouped by artist, so a global alphabetical order needs its own
 * index. One int per album — 821 albums is 3.3KB, which is why this is a sorted
 * index and not a second copy of the array. */
static int alb_order_cmp(const void *a, const void *b)
{
    const lib_album_t *x = &s_albums[*(const int *)a];
    const lib_album_t *y = &s_albums[*(const int *)b];
    /* Same bucket-then-name rule as the artists, so '#' stays contiguous and
     * "The Legendary Lost Tapes" indexes under L where you would look for it. */
    const char *kx = library_sortkey(x->name), *ky = library_sortkey(y->name);
    int bx = letter_bucket(kx), by = letter_bucket(ky);
    if (bx != by) return bx - by;
    int c = strcasecmp(kx, ky);
    return c ? c : strcasecmp(x->name, y->name);
}

static void build_album_order(void)
{
    if (s_alb_order) { heap_caps_free(s_alb_order); s_alb_order = NULL; }
    s_n_alb_order = 0;
    if (s_n_albums <= 0) return;
    s_alb_order = heap_caps_malloc((size_t)s_n_albums * sizeof(int), MALLOC_CAP_SPIRAM);
    if (!s_alb_order) s_alb_order = malloc((size_t)s_n_albums * sizeof(int));
    if (!s_alb_order) { ESP_LOGE(TAG, "no memory for the album order"); return; }
    for (int i = 0; i < s_n_albums; i++) s_alb_order[i] = i;
    qsort(s_alb_order, s_n_albums, sizeof(int), alb_order_cmp);
    s_n_alb_order = s_n_albums;
}

int library_albums_total(void) { return s_n_alb_order; }

int library_album_sorted(int i)
{
    return (i >= 0 && i < s_n_alb_order) ? s_alb_order[i] : -1;
}

char library_album_letter(int i)
{
    int g = library_album_sorted(i);
    if (g < 0) return '#';
    unsigned char c = (unsigned char)toupper(
        (unsigned char)library_sortkey(s_albums[g].name)[0]);
    return (c >= 'A' && c <= 'Z') ? (char)c : '#';
}

bool library_album_letter_range(char letter, int *first, int *count)
{
    int f = -1, n = 0;
    for (int i = 0; i < s_n_alb_order; i++) {
        if (library_album_letter(i) != letter) { if (f >= 0) break; continue; }
        if (f < 0) f = i;
        n++;
    }
    if (first) *first = (f < 0) ? 0 : f;
    if (count) *count = n;
    return f >= 0;
}

char library_album_first_letter(void)
{
    return s_n_alb_order > 0 ? library_album_letter(0) : 'A';
}

int library_track_count(int album_idx)
{
    return (album_idx >= 0 && album_idx < s_n_albums)
           ? s_albums[album_idx].n_tracks : 0;
}

const lib_track_t *library_track(int album_idx, int n)
{
    if (album_idx < 0 || album_idx >= s_n_albums) return NULL;
    if (n < 0 || n >= s_albums[album_idx].n_tracks) return NULL;
    return &s_tracks[s_albums[album_idx].first_track + n];
}

void library_track_path(int album_idx, int n, char *out, size_t len)
{
    const lib_album_t *al = &s_albums[album_idx];
    const lib_track_t *tr = &s_tracks[al->first_track + n];
    snprintf(out, len, MUSIC_DIR "/%s/%s/%s",
             s_artists[al->artist].name, al->name, tr->name);
}

/* ------------------------------------------------------------------ */

esp_err_t library_mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0  = PIN_SD_D0;
    slot.d1  = PIN_SD_D1;
    slot.d2  = PIN_SD_D2;
    slot.d3  = PIN_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "card mounted: %llu MB, %d-bit, %d kHz",
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024),
             (s_card->host.flags & SDMMC_HOST_FLAG_4BIT) ? 4 : 1,
             s_card->max_freq_khz);
    return ESP_OK;
}

/* Handing the card to USB means giving up FATFS first — and
 * esp_vfs_fat_sdcard_unmount() takes the sdmmc host down with it, which is why
 * usbmsc.c brings the card back up from scratch rather than reusing s_card. */
esp_err_t library_unmount(void)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT, s_card);
    if (err == ESP_OK) {
        s_card = NULL;
        ESP_LOGI(TAG, "card unmounted");
    } else {
        ESP_LOGE(TAG, "unmount failed: %s", esp_err_to_name(err));
    }
    return err;
}

static int cmp_name(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

/* 🔑 "The " IS NOT PART OF THE NAME FOR SORTING. 55 of the 519 artists on the
 * card begin with it; sorting on the raw name puts 76 artists under T and hides
 * "The White Stripes" there. Skipping the article spreads the worst bucket to
 * 41 and files that band under W, which is where you go looking for it.
 * ⚠️ Only the ARTIST sort uses this. Albums and tracks keep cmp_name, because
 * an album called "The Wall" should sort where it reads. */
const char *library_sortkey(const char *name)
{
    if (strncasecmp(name, "The ", 4) == 0 && name[4]) return name + 4;
    return name;
}

/* 'A'..'Z' as itself, everything else as 0 so it sorts ahead of A. */
static int letter_bucket(const char *key)
{
    unsigned char c = (unsigned char)toupper((unsigned char)key[0]);
    return (c >= 'A' && c <= 'Z') ? c : 0;
}

static int cmp_artist_name(const void *a, const void *b)
{
    /* `name` is the first member, so the struct pointer IS the name. */
    const char *ka = library_sortkey((const char *)a);
    const char *kb = library_sortkey((const char *)b);
    /* 🚨 SORT BY BUCKET FIRST, OR '#' IS NOT CONTIGUOUS. strcasecmp works on
     * bytes: '_' (0x5f) lands before 'a', a UTF-8 lead byte (>=0xc3) lands
     * after 'z'. So the non-letter artists would fall in TWO groups, one each
     * side of the alphabet, and library_letter_range() - which stops at the
     * end of the first run - would silently show only half of them. */
    int ba = letter_bucket(ka), bb = letter_bucket(kb);
    if (ba != bb) return ba - bb;
    return strcasecmp(ka, kb);
}

static bool is_mp3(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".mp3") == 0;
}

/* ---- the library cache ---------------------------------------------------
 *
 * 🔑 THE WALK IS THE SLOWEST THING THIS FIRMWARE DOES AT BOOT: 3,987 tracks
 * across 593 album directories, measured at 15.4 s on IDF 5.3 and 26.0 s on
 * 6.0.3. The three arrays it produces are flat, fixed-size, pointer-free PODs,
 * so the result is just 647 KB of bytes — one sequential read instead of
 * thousands of opendir/readdir round trips. Unit #1 has always done this; the
 * note at the top of this file has said "planned (P2)" since the scan was
 * written.
 *
 * ⚠️ A STALE CACHE IS WORSE THAN A SLOW BOOT — it shows music that is not there
 * and hides music that is. So validity is decided by looking at THE CARD, not
 * by trusting what we remember doing to it: the card's own serial and capacity,
 * plus its free space. Adding or removing anything changes free space; a
 * different card changes the serial. That catches edits made in a PC card
 * reader, which is how the music got there in the first place, and not only
 * edits made through our own USB card mode.
 *
 * 🚨 THE STRUCT SIZES ARE IN THE HEADER TOO. Adding a field to lib_track_t
 * would otherwise reinterpret 647 KB of old bytes as the new layout and produce
 * a library of garbage after an innocent firmware update. */
#define LIBCACHE_PATH  SD_MOUNT "/.libcache"
#define LIBCACHE_MAGIC 0x4C494243u      /* "LIBC" */
#define LIBCACHE_VER   2u      /* 2: artists sorted by library_sortkey()
                                 * (leading "The " ignored). A v1 cache holds
                                 * the OLD order and must be rejected, or the
                                 * A-Z index points at the wrong rows. */

typedef struct {
    uint32_t magic, version;
    uint32_t sz_artist, sz_album, sz_track;   /* layout guard */
    int32_t  cid_serial;
    uint64_t capacity_bytes;
    uint64_t free_bytes;
    int32_t  n_artists, n_albums, n_tracks;
} libcache_hdr_t;

static uint64_t card_capacity_bytes(void)
{
    if (!s_card) return 0;
    return (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
}

/* Free space, as FATFS reports it. Cheap: FAT32 keeps the free count in FSINFO
 * rather than walking the allocation table. */
static uint64_t card_free_bytes(void)
{
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(SD_MOUNT, &total, &freeb) != ESP_OK) return UINT64_MAX;
    return freeb;
}

static void libcache_fill(libcache_hdr_t *h)
{
    memset(h, 0, sizeof *h);
    h->magic          = LIBCACHE_MAGIC;
    h->version        = LIBCACHE_VER;
    h->sz_artist      = sizeof(lib_artist_t);
    h->sz_album       = sizeof(lib_album_t);
    h->sz_track       = sizeof(lib_track_t);
    h->cid_serial     = s_card ? s_card->cid.serial : 0;
    h->capacity_bytes = card_capacity_bytes();
    h->free_bytes     = card_free_bytes();
    h->n_artists      = s_n_artists;
    h->n_albums       = s_n_albums;
    h->n_tracks       = s_n_tracks;
}

/* Returns ESP_OK only when the cache was read AND matches this card. */
static esp_err_t libcache_load(void)
{
    FILE *f = fopen(LIBCACHE_PATH, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    libcache_hdr_t h, want;
    if (fread(&h, 1, sizeof h, f) != sizeof h) { fclose(f); return ESP_ERR_INVALID_SIZE; }
    libcache_fill(&want);          /* current card, current layout */

    const char *why = NULL;
    if (h.magic != LIBCACHE_MAGIC || h.version != LIBCACHE_VER) why = "wrong magic or version";
    else if (h.sz_artist != want.sz_artist || h.sz_album != want.sz_album ||
             h.sz_track  != want.sz_track)                      why = "struct layout changed";
    else if (h.cid_serial != want.cid_serial)                   why = "different card";
    else if (h.capacity_bytes != want.capacity_bytes)           why = "different capacity";
    else if (h.free_bytes != want.free_bytes)                   why = "card contents changed";
    else if (h.n_artists <= 0 || h.n_artists > MAX_ARTISTS ||
             h.n_albums  <= 0 || h.n_albums  > MAX_ALBUMS  ||
             h.n_tracks  <= 0 || h.n_tracks  > MAX_TRACKS)      why = "counts out of range";
    if (why) { ESP_LOGI(TAG, "cache miss: %s", why); fclose(f); return ESP_ERR_INVALID_STATE; }

    s_artists = heap_caps_calloc(MAX_ARTISTS, sizeof(lib_artist_t), MALLOC_CAP_SPIRAM);
    s_albums  = heap_caps_calloc(MAX_ALBUMS,  sizeof(lib_album_t),  MALLOC_CAP_SPIRAM);
    s_tracks  = heap_caps_calloc(MAX_TRACKS,  sizeof(lib_track_t),  MALLOC_CAP_SPIRAM);
    if (!s_artists || !s_albums || !s_tracks) { fclose(f); return ESP_ERR_NO_MEM; }

    bool ok = fread(s_artists, sizeof(lib_artist_t), h.n_artists, f) == (size_t)h.n_artists &&
              fread(s_albums,  sizeof(lib_album_t),  h.n_albums,  f) == (size_t)h.n_albums  &&
              fread(s_tracks,  sizeof(lib_track_t),  h.n_tracks,  f) == (size_t)h.n_tracks;
    fclose(f);
    if (!ok) { ESP_LOGW(TAG, "cache truncated - rescanning"); return ESP_ERR_INVALID_SIZE; }

    s_n_artists = h.n_artists;
    s_n_albums  = h.n_albums;
    s_n_tracks  = h.n_tracks;
    return ESP_OK;
}

static void libcache_save(void)
{
    FILE *f = fopen(LIBCACHE_PATH, "wb");
    if (!f) { ESP_LOGW(TAG, "cannot write %s", LIBCACHE_PATH); return; }

    libcache_hdr_t h;
    libcache_fill(&h);
    /* ⚠️ The header is written with the free space AS IT IS AFTER this file
     * exists, because that is the state the next boot will observe. Writing the
     * pre-write value would miss by exactly the size of the cache and every
     * boot would rescan. The file is created first, then rewound and stamped. */
    bool ok = fwrite(&h, 1, sizeof h, f) == sizeof h &&
              fwrite(s_artists, sizeof(lib_artist_t), s_n_artists, f) == (size_t)s_n_artists &&
              fwrite(s_albums,  sizeof(lib_album_t),  s_n_albums,  f) == (size_t)s_n_albums  &&
              fwrite(s_tracks,  sizeof(lib_track_t),  s_n_tracks,  f) == (size_t)s_n_tracks;
    fclose(f);
    if (!ok) { ESP_LOGW(TAG, "cache write failed - removing"); unlink(LIBCACHE_PATH); return; }

    /* Now that the file occupies its final size, restamp the header with the
     * free space the NEXT boot will actually see. */
    f = fopen(LIBCACHE_PATH, "r+b");
    if (f) {
        libcache_fill(&h);
        fwrite(&h, 1, sizeof h, f);
        fclose(f);
    }
    ESP_LOGI(TAG, "cache written: %d/%d/%d", s_n_artists, s_n_albums, s_n_tracks);
}

void library_invalidate_cache(void)
{
    if (unlink(LIBCACHE_PATH) == 0) ESP_LOGI(TAG, "cache dropped - next boot rescans");
}

esp_err_t library_scan(library_progress_cb cb)
{
    int64_t t0 = esp_timer_get_time();

    if (libcache_load() == ESP_OK) {
        ESP_LOGI(TAG, "loaded %d artists / %d albums / %d tracks from cache in %lld ms",
                 s_n_artists, s_n_albums, s_n_tracks,
                 (long long)((esp_timer_get_time() - t0) / 1000));
        build_album_order();
        if (cb) cb(s_n_artists, s_n_albums, s_n_tracks);
        return ESP_OK;
    }
    /* A miss leaves the arrays either NULL or partly allocated; start clean. */
    if (s_artists) { heap_caps_free(s_artists); s_artists = NULL; }
    if (s_albums)  { heap_caps_free(s_albums);  s_albums  = NULL; }
    if (s_tracks)  { heap_caps_free(s_tracks);  s_tracks  = NULL; }
    s_n_artists = s_n_albums = s_n_tracks = 0;

    s_artists = heap_caps_calloc(MAX_ARTISTS, sizeof(lib_artist_t), MALLOC_CAP_SPIRAM);
    s_albums  = heap_caps_calloc(MAX_ALBUMS,  sizeof(lib_album_t),  MALLOC_CAP_SPIRAM);
    s_tracks  = heap_caps_calloc(MAX_TRACKS,  sizeof(lib_track_t),  MALLOC_CAP_SPIRAM);
    if (!s_artists || !s_albums || !s_tracks) {
        ESP_LOGE(TAG, "library allocation failed — is PSRAM enabled?");
        return ESP_ERR_NO_MEM;
    }

    DIR *ad = opendir(MUSIC_DIR);
    if (!ad) {
        ESP_LOGE(TAG, "no " MUSIC_DIR " — is the card written?");
        return ESP_ERR_NOT_FOUND;
    }

    struct dirent *ae;
    while ((ae = readdir(ad)) && s_n_artists < MAX_ARTISTS) {
        if (ae->d_name[0] == '.') continue;

        char apath[300];
        snprintf(apath, sizeof(apath), MUSIC_DIR "/%s", ae->d_name);
        DIR *bd = opendir(apath);
        if (!bd) continue;

        lib_artist_t *ar = &s_artists[s_n_artists];
        strlcpy(ar->name, ae->d_name, sizeof(ar->name));
        ar->first_album = s_n_albums;
        ar->n_albums = 0;
        ar->n_tracks = 0;

        struct dirent *be;
        while ((be = readdir(bd)) && s_n_albums < MAX_ALBUMS) {
            if (be->d_name[0] == '.') continue;

            char bpath[600];
            snprintf(bpath, sizeof(bpath), "%s/%s", apath, be->d_name);
            DIR *td = opendir(bpath);
            if (!td) continue;

            lib_album_t *al = &s_albums[s_n_albums];
            strlcpy(al->name, be->d_name, sizeof(al->name));
            al->artist = s_n_artists;
            al->first_track = s_n_tracks;
            al->n_tracks = 0;

            struct dirent *te;
            while ((te = readdir(td)) && s_n_tracks < MAX_TRACKS) {
                if (te->d_name[0] == '.' || !is_mp3(te->d_name)) continue;
                strlcpy(s_tracks[s_n_tracks].name, te->d_name,
                        sizeof(s_tracks[s_n_tracks].name));
                s_tracks[s_n_tracks].album = s_n_albums;
                s_n_tracks++;
                al->n_tracks++;
            }
            closedir(td);

            if (al->n_tracks == 0) continue;      /* skip empty folders */

            /* Track order is what a kid expects: "01 ..." before "02 ...". */
            qsort(&s_tracks[al->first_track], al->n_tracks,
                  sizeof(lib_track_t), cmp_name);

            ar->n_tracks += al->n_tracks;
            ar->n_albums++;
            s_n_albums++;
        }
        closedir(bd);

        if (ar->n_albums == 0) continue;
        qsort(&s_albums[ar->first_album], ar->n_albums,
              sizeof(lib_album_t), cmp_name);
        s_n_artists++;

        if (cb) cb(s_n_artists, s_n_albums, s_n_tracks);
    }
    closedir(ad);

    qsort(s_artists, s_n_artists, sizeof(lib_artist_t), cmp_artist_name);

    /* Sorting the artist array moved the rows, so every album's back-reference
     * to its artist is now stale. Rebuild them from the album ranges. */
    for (int i = 0; i < s_n_artists; i++) {
        for (int j = 0; j < s_artists[i].n_albums; j++) {
            s_albums[s_artists[i].first_album + j].artist = i;
        }
    }

    ESP_LOGI(TAG, "scanned %d artists / %d albums / %d tracks in %lld ms",
             s_n_artists, s_n_albums, s_n_tracks,
             (esp_timer_get_time() - t0) / 1000);

    build_album_order();

    /* Written AFTER the sorts, so the cache holds the finished arrays in the
     * order the UI expects - not the raw readdir order. */
    libcache_save();
    return ESP_OK;
}
