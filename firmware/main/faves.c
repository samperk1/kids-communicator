/* Favourites: a hearted-song list that survives a card rebuild.
 *
 * See faves.h for why the unit of storage is a path and not an index.
 *
 * ⚠️ THE WRITE IS DEFERRED ON PURPOSE. This card is also the source of the mp3
 * being streamed, and SD contention on this hardware is a known way to make
 * audio stutter (a lesson from an earlier CrowPanel build). A heart tap therefore only
 * sets a dirty flag; the file is written by faves_flush(), which the UI calls
 * when it leaves the player or when playback stops. The list is small and the
 * write is one short file, so a flush costs a few milliseconds when it happens.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "hw.h"
#include "library.h"
#include "faves.h"

static const char *TAG = "faves";

#define FAVES_PATH   SD_MOUNT "/favourites.m3u"
#define FAVES_MAX    512
/* 🚨 BIG ENOUGH FOR THE WORST CASE, BECAUSE TRUNCATION IS NOT A SHORTER PATH,
 * IT IS A DIFFERENT SONG. lib_artist_t, lib_album_t and lib_track_t each hold a
 * 128-byte name, so "<Artist>/<Album>/<Track>.mp3" can reach ~384. A 192-byte
 * buffer silently cut long paths and every one of them would have resolved to
 * nothing, or worse, collided with another truncated path. */
#define RELMAX       384          /* "<Artist>/<Album>/<Track>.mp3" */

typedef struct {
    char rel[RELMAX];             /* path under MUSIC_DIR — the identity */
    int  album;                   /* resolved index, or -1 when not on this card */
    int  track;
} fave_t;

static fave_t *s_f;               /* FAVES_MAX entries, PSRAM */
static int     s_n;
static bool    s_dirty;

/* The resolved subset, rebuilt by resolve(). The shuffle scope is this, not
 * s_n: shuffling over favourites that are not on the card would pick nothing. */
static int s_res[FAVES_MAX];
static int s_n_res;

static bool ensure(void)
{
    if (s_f) return true;
    s_f = heap_caps_calloc(FAVES_MAX, sizeof(fave_t), MALLOC_CAP_SPIRAM);
    if (!s_f) s_f = calloc(FAVES_MAX, sizeof(fave_t));
    if (!s_f) { ESP_LOGE(TAG, "no memory for favourites"); return false; }
    return true;
}

/* "<Artist>/<Album>/<Track>" — library_track_path() with MUSIC_DIR removed, so
 * the file stays readable and stays valid if the mount point ever changes. */
static void rel_of(int album_idx, int track, char *out, size_t len)
{
    char full[RELMAX + 64];
    library_track_path(album_idx, track, full, sizeof(full));
    const char *p = full;
    size_t pre = strlen(MUSIC_DIR "/");
    if (strncmp(full, MUSIC_DIR "/", pre) == 0) p = full + pre;
    snprintf(out, len, "%s", p);
}

static int find(const char *rel)
{
    for (int i = 0; i < s_n; i++)
        if (strcmp(s_f[i].rel, rel) == 0) return i;
    return -1;
}

/* Rebuild the resolved list from s_f alone. s_f already carries each entry's
 * album/track, so this needs no library walk — O(favourites), not O(tracks). */
static void reindex_resolved(void)
{
    s_n_res = 0;
    for (int i = 0; i < s_n; i++)
        if (s_f[i].album >= 0 && s_n_res < FAVES_MAX) s_res[s_n_res++] = i;
}

/* Re-derive every index from the paths, by walking the library. Runs after a
 * SCAN only — see faves_toggle() for why it must not run on a tap. */
static void resolve(void)
{
    for (int i = 0; i < s_n; i++) { s_f[i].album = -1; s_f[i].track = -1; }
    s_n_res = 0;

    /* One pass over the library building each track's path, rather than a
     * lookup per favourite: the library is the big side, and this way it is
     * walked once however many favourites there are. */
    int n_art = library_artist_count();
    for (int a = 0; a < n_art; a++) {
        const lib_artist_t *ar = library_artist(a);
        if (!ar) continue;
        for (int b = 0; b < ar->n_albums; b++) {
            int alb = ar->first_album + b;
            int nt  = library_track_count(alb);
            for (int t = 0; t < nt; t++) {
                char rel[RELMAX];
                rel_of(alb, t, rel, sizeof(rel));
                int i = find(rel);
                if (i < 0) continue;
                s_f[i].album = alb;
                s_f[i].track = t;
            }
        }
    }
    reindex_resolved();
    ESP_LOGI(TAG, "%d favourites, %d on this card", s_n, s_n_res);
}

void faves_load(void)
{
    if (!ensure()) return;
    s_n = 0; s_n_res = 0; s_dirty = false;

    FILE *fp = fopen(FAVES_PATH, "r");
    if (!fp) { ESP_LOGI(TAG, "no %s yet", FAVES_PATH); return; }
    char line[RELMAX + 64];
    while (fgets(line, sizeof(line), fp) && s_n < FAVES_MAX) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;       /* m3u comments */
        if (strlen(line) >= RELMAX) {          /* never truncate — see RELMAX */
            ESP_LOGW(TAG, "path too long, skipped: %.48s...", line);
            continue;
        }
        if (find(line) >= 0) continue;                  /* tolerate duplicates */
        strlcpy(s_f[s_n].rel, line, RELMAX);
        s_f[s_n].album = -1; s_f[s_n].track = -1;
        s_n++;
    }
    fclose(fp);
    resolve();
}

void faves_flush(void)
{
    if (!s_dirty || !s_f) return;
    FILE *fp = fopen(FAVES_PATH, "w");
    if (!fp) { ESP_LOGW(TAG, "cannot write %s", FAVES_PATH); return; }
    fputs("#EXTM3U\n", fp);
    /* Every path is written back, including the ones this card cannot resolve —
     * see faves.h. Losing another handset's favourites by inserting its card
     * would be silent and unrecoverable. */
    for (int i = 0; i < s_n; i++) fprintf(fp, "%s\n", s_f[i].rel);
    fclose(fp);
    s_dirty = false;
    ESP_LOGI(TAG, "wrote %d favourites", s_n);
}

bool faves_contains(int album_idx, int track)
{
    if (!s_f) return false;
    char rel[RELMAX];
    rel_of(album_idx, track, rel, sizeof(rel));
    return find(rel) >= 0;
}

bool faves_toggle(int album_idx, int track)
{
    if (!ensure()) return false;
    char rel[RELMAX];
    rel_of(album_idx, track, rel, sizeof(rel));
    int i = find(rel);
    if (i >= 0) {
        memmove(&s_f[i], &s_f[i + 1], (size_t)(s_n - i - 1) * sizeof(fave_t));
        s_n--;
    } else {
        if (s_n >= FAVES_MAX) { ESP_LOGW(TAG, "favourites full"); return false; }
        strlcpy(s_f[s_n].rel, rel, RELMAX);
        s_f[s_n].album = album_idx; s_f[s_n].track = track;
        s_n++;
    }
    s_dirty = true;
    /* 🚨 NOT resolve() — THIS RUNS IN A TOUCH HANDLER. resolve() walks every
     * track on the card building a path for each; on a 5,180-track card that is
     * hundreds of milliseconds of SD-backed work inside the UI event, which is
     * exactly the blocking-handler mistake this project has already shipped
     * once. Nothing needs the library here: the entry's album/track are known,
     * so only the resolved index list has to be rebuilt, and that is O(n) over
     * at most 512 entries. */
    reindex_resolved();
    return i < 0;
}

int faves_count(void) { return s_n_res; }

bool faves_pick(int g, int *album_out, int *track_out)
{
    if (g < 0 || g >= s_n_res) return false;
    const fave_t *f = &s_f[s_res[g]];
    if (f->album < 0) return false;
    if (album_out) *album_out = f->album;
    if (track_out) *track_out = f->track;
    return true;
}
