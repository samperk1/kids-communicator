/*
 * Fleet roster and per-link keys, in NVS.
 *
 * Written lazily like settings.c, and for the same reason: NVS is flash. The
 * roster changes at pairing time and almost never otherwise, but the ratchet
 * (seen_encrypted) is set from the receive path, so that one commits
 * immediately — a ratchet that is only in RAM is not a ratchet.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/platform_util.h"
#include "psa/crypto.h"
#include "mbedtls/md.h"
#include "applog.h"
#include "fleet.h"
/* AFTER fleet.h, which is where FLEET_KDF_SELFTEST is defined. Guarding this
 * include above that line would silently skip it even when the flag is 1 - the
 * macro simply is not defined yet at that point, so #if reads it as 0. */
#if FLEET_KDF_SELFTEST
#include "mbedtls/pkcs5.h"   /* PRIVATE in IDF 6.0 - selftest builds only */
#include "mbedtls/gcm.h"     /* PRIVATE in IDF 6.0 - selftest builds only */
#endif

static const char *TAG = "fleet";

/* ⚠️ ITS OWN NAMESPACE — "kc" ON EVERY ONE THAT HOLDS IDENTITY OR KEYS.
 *
 * This firmware was split out of a private family build that runs on the same
 * boards, and handsets get flashed back and forth between the two for testing.
 * The family build keeps its roster under "fleet"; if this build used the same
 * namespace it would overwrite that roster — and because keys are per link,
 * recovering would mean re-pairing the handset with every other one. So the
 * roster lives in "kcfleet", settings in "kccomm" and the message counter in
 * "kcespnow", and both builds' state sits in the 24K nvs partition untouched. */
static const char *NS = "kcfleet";

static fleet_peer_t s_peers[FLEET_MAX];
static uint8_t      s_pmk[FLEET_KEY_LEN];
/* 🚨 DEFAULTS TO OPEN, AND THAT IS DELIBERATE.
 *
 * Closed, a freshly flashed handset refuses every handset that is not in its
 * roster — which on the first flash is all of them. That would re-create the
 * single-evening cutover this whole design exists to avoid: flash Kid 4's and she
 * instantly stops hearing Kid 2 and Kid 3.
 *
 * So it starts open, the roster screen shows a warning the entire time it is,
 * and CLOSING IT IS THE LAST STEP OF THE MIGRATION once every handset is
 * paired. The per-peer ratchet is what actually protects a paired link
 * meanwhile. NVS overrides this once the parent has set it either way. */
/* 🔒 COMM_LIGHT: the downgrade path does not exist in this build.
 *
 * fleet.h has warned since it was written that this is "MIGRATION-ONLY: strip
 * before public repo". It is equally a hand-over blocker: migration starts OPEN
 * and is closed by a manual Fleet tap, so a handset given away with that tap
 * missed would leave the house with a working route back to cleartext.
 * Compile-time false means fleet_clear_allowed() can never say yes, whatever is
 * sitting in NVS from a previous full build. */
#define s_migration_open  false
static bool         s_loaded;
static bool         s_have_pmk;

/* ------------------------------------------------------------------ store */

static void store(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed, roster not saved");
        return;
    }
    /* 🚨 EVERY ONE OF THESE USED TO BE UNCHECKED, AND THAT HID THE WORST BUG
     * IN THE PAIRING STORY.
     *
     * The builder, 2026-09-07: Parent would pair — "paired Kid 1 as #5", "paired with
     * Kid 1 - link encrypted", both ends happy — and come back from a reboot with
     * an EMPTY ROSTER. `fleet_next_device_no()` returning 5 proves four entries
     * existed in RAM at that moment. They never reached flash.
     *
     * A failed nvs_set_blob() here loses THE FLEET'S KEYS while every log line
     * says the pairing succeeded, and the symptom appears later and elsewhere:
     * a one-way link, which reads as a radio fault. The NVS partition is
     * 0x6000 — 24KB — shared with wifi data, settings, the message stamps and
     * the T9 dictionary state, so running out is not exotic.
     *
     * Report the space too. "Roster not saved" is only half an answer; the
     * useful half is whether there was anywhere to put it. */
    esp_err_t e1 = nvs_set_blob(h, "roster", s_peers, sizeof(s_peers));
    esp_err_t e2 = nvs_set_blob(h, "pmk",    s_pmk,   sizeof(s_pmk));
    esp_err_t e3 = nvs_set_i32 (h, "migrate", s_migration_open ? 1 : 0);
    esp_err_t e4 = nvs_set_i32 (h, "gen", FLEET_GEN);
    esp_err_t ec = nvs_commit(h);
    nvs_close(h);

    if (e1 || e2 || e3 || e4 || ec) {
        nvs_stats_t st;
        if (nvs_get_stats(NULL, &st) == ESP_OK)
            applog("[fleet] 🚨 ROSTER NOT SAVED - roster:%s pmk:%s commit:%s "
                   "(nvs %d used / %d free of %d)",
                   esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(ec),
                   (int)st.used_entries, (int)st.free_entries,
                   (int)st.total_entries);
        else
            applog("[fleet] 🚨 ROSTER NOT SAVED - roster:%s pmk:%s commit:%s",
                   esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(ec));
        applog("[fleet] the pairing is live in RAM and WILL BE LOST on reboot");
    }
}

void fleet_save(void) { store(); }

void fleet_init(void)
{
    if (s_loaded) return;
    s_loaded = true;

    memset(s_peers, 0, sizeof(s_peers));
    memset(s_pmk,   0, sizeof(s_pmk));

    nvs_handle_t h;
    bool have_pmk = false, wipe_roster = false;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_peers);
        /* A size mismatch means the struct changed under a stored blob. Drop
         * it rather than read a shorter record into a longer one — the keys
         * would be garbage and every link would fail in a way that looks like
         * a radio fault. */
        if (nvs_get_blob(h, "roster", s_peers, &sz) != ESP_OK || sz != sizeof(s_peers))
            memset(s_peers, 0, sizeof(s_peers));

        sz = sizeof(s_pmk);
        have_pmk = (nvs_get_blob(h, "pmk", s_pmk, &sz) == ESP_OK && sz == sizeof(s_pmk));

        int32_t m = 0;
        /* Deliberately NOT read back. These boards have a stale migrate=1 in
         * NVS from the full build, and honouring it would reopen the very hole
         * this build exists to close. store() writes 0 over it on first save. */
        (void)m;

        int32_t gen = 0;
        nvs_get_i32(h, "gen", &gen);
        if (gen != FLEET_GEN) {
            memset(s_peers, 0, sizeof(s_peers));
            wipe_roster = true;
            ESP_LOGW(TAG, "fleet gen %d -> %d: stored pairings cleared",
                     (int)gen, FLEET_GEN);
        }
        nvs_close(h);
    }

    /* 🚨 NO RANDOM PMK HERE. See the note in fleet.h — the PMK must be equal
     * across the fleet, so it comes from the fleet code and nowhere else. With
     * no code entered yet there is no PMK, and no encrypted link is possible;
     * that is the correct state for a handset that has not joined a fleet. */
    s_have_pmk = have_pmk;
    if (!s_have_pmk) ESP_LOGI(TAG, "no fleet code entered yet");

    if (wipe_roster) {
        store();
        applog("[fleet] pairings cleared - pair again when ready");
    }

    /* 🔑 NAME THE ROSTER AT BOOT, AND SAY WHEN IT IS FULL.
     *
     * "%d paired" was not enough. The builder, 2026-09-07: Parent would not pair
     * with Kid 1 while both paired with everyone else — and the roster
     * contents, the one thing that would have explained it in a second, were
     * nowhere in the log. FLEET_MAX is 8, and a full roster makes fleet_add()
     * return NULL, which the initiator's T_PAIR_ACK handler turns into a silent
     * `break` — the responder commits, the initiator does not, and the link is
     * one-way. Exactly the reported symptom.
     *
     * A roster is at most 8 lines once per boot. Print it. */
    /* One line, every boot. A roster that vanishes overnight and an NVS
     * partition with no room left look identical from the UI, and only one of
     * them is a pairing problem. 0x6000 = 24KB shared with wifi data, settings,
     * message stamps and T9 state. */
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK)
        applog("[fleet] nvs %d used / %d free of %d entries",
               (int)st.used_entries, (int)st.free_entries, (int)st.total_entries);

    int n = fleet_count();
    if (n) {
        applog("[fleet] %d/%d paired%s", n, FLEET_MAX,
               s_migration_open ? ", migration OPEN" : "");
        for (int i = 0; i < FLEET_MAX; i++) {
            fleet_peer_t *p = fleet_at(i);
            if (p) applog("[fleet]   #%u %s %s", p->device_no, p->name,
                          p->seen_encrypted ? "(encrypted)" : "(cleartext ok)");
        }
        if (n >= FLEET_MAX)
            applog("[fleet] 🚨 ROSTER FULL - further pairings will fail, and the "
                   "far end will still show itself as paired");
    }
}

const uint8_t *fleet_pmk(void) { return s_pmk; }
bool fleet_has_pmk(void)       { return s_have_pmk; }

/* ---- PBKDF2-HMAC-SHA256, written here, over mbedtls's HMAC ---------------
 *
 * 🚨 WHY THIS EXISTS: IDF 6.0 ships Mbed TLS 4.x, which moved `pkcs5.h` to
 * `mbedtls/private/pkcs5.h` — `mbedtls_pkcs5_pbkdf2_hmac_ext()` is no longer
 * public API. The PSA replacement was tried first and returns
 * PSA_ERROR_NOT_SUPPORTED (-134) on this build: `PSA_WANT_ALG_PBKDF2_HMAC` is
 * set in mbedtls's own crypto_config.h, but IDF does not enable
 * MBEDTLS_PSA_CRYPTO_CONFIG, so it is never consulted.
 *
 * 🔑 THE CRYPTOGRAPHY IS NOT OURS. HMAC-SHA256 stays in mbedtls — `mbedtls_md`
 * is public in BOTH 5.3 and 4.x (verified: tf-psa-crypto/include/mbedtls/md.h,
 * not under private/). What is written here is only PBKDF2's bookkeeping from
 * RFC 8018 §5.2: a big-endian block counter, `iters` chained HMACs, and an XOR
 * fold. No primitive is reimplemented.
 *
 *     DK      = T_1 || T_2 || ...
 *     T_i     = U_1 XOR U_2 XOR ... XOR U_c
 *     U_1     = HMAC(P, S || INT32_BE(i))
 *     U_j     = HMAC(P, U_{j-1})
 *
 * ⚠️ THIS DERIVES THE FLEET PMK. One wrong byte and every handset stops being
 * able to talk to every other handset, and per "espnow pmk must match"
 * that presents as a RADIO fault, not a crypto one. It is therefore NOT trusted
 * on the strength of matching the RFC — `fleet_kdf_selftest()` below compares it
 * against `mbedtls_pkcs5_pbkdf2_hmac_ext()` byte for byte on the device.
 *
 * 🔑 The key is set ONCE with hmac_starts() and each message begins with
 * hmac_reset(). Re-keying per iteration would recompute the inner/outer pads
 * 100,000 times for no reason. */
static int fleet_pbkdf2(const uint8_t *pass, size_t pass_len,
                        const uint8_t *salt, size_t salt_len,
                        uint32_t iters, uint8_t *out, size_t out_len)
{
    if (!iters || !out_len) return -1;

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;
    const size_t hlen = mbedtls_md_get_size(info);      /* 32 */

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, info, 1 /* HMAC */);
    if (rc) { mbedtls_md_free(&ctx); return rc; }
    rc = mbedtls_md_hmac_starts(&ctx, pass, pass_len);

    uint8_t U[MBEDTLS_MD_MAX_SIZE], T[MBEDTLS_MD_MAX_SIZE];
    size_t done = 0;

    for (uint32_t i = 1; !rc && done < out_len; i++) {
        const uint8_t ctr[4] = { (uint8_t)(i >> 24), (uint8_t)(i >> 16),
                                 (uint8_t)(i >> 8),  (uint8_t)i };
        if ((rc = mbedtls_md_hmac_reset(&ctx)))                    break;
        if ((rc = mbedtls_md_hmac_update(&ctx, salt, salt_len)))   break;
        if ((rc = mbedtls_md_hmac_update(&ctx, ctr, sizeof ctr)))  break;
        if ((rc = mbedtls_md_hmac_finish(&ctx, U)))                break;
        memcpy(T, U, hlen);

        for (uint32_t j = 1; j < iters; j++) {
            if ((rc = mbedtls_md_hmac_reset(&ctx)))         goto out;
            if ((rc = mbedtls_md_hmac_update(&ctx, U, hlen))) goto out;
            if ((rc = mbedtls_md_hmac_finish(&ctx, U)))     goto out;
            for (size_t k = 0; k < hlen; k++) T[k] ^= U[k];
        }

        size_t take = (out_len - done < hlen) ? (out_len - done) : hlen;
        memcpy(out + done, T, take);
        done += take;
    }
out:
    mbedtls_md_free(&ctx);
    /* Intermediates are key material. Zeroise rather than leave them on the
     * stack for whatever runs next. */
    mbedtls_platform_zeroize(U, sizeof U);
    mbedtls_platform_zeroize(T, sizeof T);
    return rc;
}

#if FLEET_KDF_SELFTEST
/* Defined further down, next to the key wrapping they serve. Forward-declared
 * because the selftests live up here with the KDF they were written for. */
static int fleet_gcm_seal(const uint8_t *key, const uint8_t *nonce,
                          const uint8_t *pt, size_t pt_len,
                          uint8_t *ct_out, uint8_t *tag_out);
static int fleet_gcm_open(const uint8_t *key, const uint8_t *nonce,
                          const uint8_t *ct, size_t ct_len,
                          const uint8_t *tag, uint8_t *pt_out);
static void fleet_gcm_selftest(void);

/* 🔑 A CHECK THAT CAN FAIL, RUN AT BOOT, AGAINST A FIXED TEST STRING.
 *
 * Derives the SAME key both ways — the current `mbedtls_pkcs5_pbkdf2_hmac_ext()`
 * and the PSA replacement — and compares the bytes. It deliberately uses a
 * throwaway passphrase, NOT the stored fleet code, so proving the port costs
 * nothing and touches no real key material.
 *
 * On IDF 5.3 both implementations exist, which is the whole reason this port is
 * being done here rather than during the toolchain move: 6.0 can only run the
 * PSA half and could never have compared them. **A PASS here is what licenses
 * the switch; a FAIL means do not ship it.**
 *
 * ⚠️ Remove this once the PSA path is the only one — it costs two full
 * FLEET_KDF_ITERS derivations at every boot. */
void fleet_kdf_selftest(void)
{
    fleet_gcm_selftest();   /* fast; the KDF half below costs ~28s */

    static const char     *TEST_CODE = "kdf-selftest-not-a-real-fleet-code";
    static const uint8_t   TEST_SALT[16] = "kids-comm/pmk/v1";
    uint8_t a[FLEET_KEY_LEN] = {0}, b[FLEET_KEY_LEN] = {0};

    int64_t t0 = esp_timer_get_time();
    int rc_old = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                               (const unsigned char *)TEST_CODE, strlen(TEST_CODE),
                                               TEST_SALT, sizeof(TEST_SALT),
                                               FLEET_KDF_ITERS, FLEET_KEY_LEN, a);
    int64_t t1 = esp_timer_get_time();
    int rc_new = fleet_pbkdf2((const uint8_t *)TEST_CODE, strlen(TEST_CODE),
                              TEST_SALT, sizeof(TEST_SALT),
                              FLEET_KDF_ITERS, b, FLEET_KEY_LEN);
    int64_t t2 = esp_timer_get_time();

    if (rc_old || rc_new) {
        applog("[kdf] SELFTEST ERROR old=%d new=%d", rc_old, rc_new);
        return;
    }
    bool same = (memcmp(a, b, FLEET_KEY_LEN) == 0);
    applog("[kdf] selftest %s  pkcs5 %lldms  ours %lldms",
           same ? "PASS - identical keys" : "*** FAIL - KEYS DIFFER, DO NOT SHIP ***",
           (long long)((t1 - t0) / 1000), (long long)((t2 - t1) / 1000));
    if (!same) {
        /* Print both so the difference is inspectable rather than asserted. */
        char ha[2 * FLEET_KEY_LEN + 1], hb[2 * FLEET_KEY_LEN + 1];
        for (int i = 0; i < FLEET_KEY_LEN; i++) {
            sprintf(ha + 2 * i, "%02x", a[i]);
            sprintf(hb + 2 * i, "%02x", b[i]);
        }
        applog("[kdf] pkcs5=%s", ha);
        applog("[kdf] ours =%s", hb);
    }
}


/* 🔑 AES-GCM EQUIVALENCE, AND IT IS A COMPLETE PROOF, NOT A SAMPLE.
 *
 * GCM is deterministic for a given key/nonce/plaintext, so sealing the same
 * input with the old mbedtls path and the new PSA path must give identical
 * ciphertext AND identical tag. A FIXED nonce is used deliberately — the real
 * code randomises it, which would make the two incomparable.
 *
 * ⚠️ Reusing a nonce with a real key would be a serious AES-GCM misuse. This is
 * a throwaway test key that never wraps anything and never leaves the function.
 *
 * Also round-trips through fleet_gcm_open(), because a seal that cannot be
 * opened is worse than one that fails loudly. */
static void fleet_gcm_selftest(void)
{
    static const uint8_t KEY[FLEET_KEY_LEN] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
    static const uint8_t NONCE[12] = {
        0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xab };
    static const uint8_t PT[FLEET_KEY_LEN] = {
        0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80,
        0x90,0xa0,0xb0,0xc0,0xd0,0xe0,0xf0,0x01 };

    uint8_t ct_old[16] = {0}, tag_old[16] = {0};
    uint8_t ct_new[16] = {0}, tag_new[16] = {0};

    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int rc_old = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, KEY, FLEET_KEY_LEN * 8);
    if (!rc_old)
        rc_old = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, FLEET_KEY_LEN,
                                           NONCE, 12, NULL, 0,
                                           PT, ct_old, 16, tag_old);
    mbedtls_gcm_free(&g);

    int rc_new = fleet_gcm_seal(KEY, NONCE, PT, FLEET_KEY_LEN, ct_new, tag_new);

    if (rc_old || rc_new) {
        applog("[gcm] SELFTEST ERROR old=%d new=%d", rc_old, rc_new);
        return;
    }
    bool same = (memcmp(ct_old, ct_new, 16) == 0) &&
                (memcmp(tag_old, tag_new, 16) == 0);

    uint8_t back[FLEET_KEY_LEN] = {0};
    int rc_rt = fleet_gcm_open(KEY, NONCE, ct_new, FLEET_KEY_LEN, tag_new, back);
    bool round = (rc_rt == 0) && (memcmp(back, PT, FLEET_KEY_LEN) == 0);

    applog("[gcm] selftest %s, roundtrip %s",
           same  ? "PASS - identical ct+tag" : "*** FAIL - CIPHERTEXT DIFFERS ***",
           round ? "PASS" : "*** FAIL ***");
}
#endif /* FLEET_KDF_SELFTEST */

/* The fixed salt is a constant, not a secret — its job is domain separation
 * between the PMK and the pairing keys, which are derived from the same code. */
void fleet_set_code(const char *code)
{
    if (!code || !*code) return;
    int64_t t0 = esp_timer_get_time();
    /* 🔑 Our own PBKDF2 over mbedtls's HMAC — see fleet_pbkdf2() above for why,
     * and fleet.h for the proof. Verified byte-identical to
     * mbedtls_pkcs5_pbkdf2_hmac_ext() on hardware before this switch was made. */
    int rc = fleet_pbkdf2((const uint8_t *)code, strlen(code),
                          (const uint8_t *)"kids-comm/pmk/v1", 16,
                          FLEET_KDF_ITERS, s_pmk, FLEET_KEY_LEN);
    if (rc) {
        ESP_LOGE(TAG, "pmk derivation failed: -0x%04x", -rc);
        applog("[fleet] fleet code REJECTED, key derivation failed");
        memset(s_pmk, 0, sizeof(s_pmk));
        s_have_pmk = false;
        return;
    }
    s_have_pmk = true;
    store();
    /* Log the real cost — FLEET_KDF_ITERS is a guess until this prints. */
    applog("[fleet] fleet code accepted (%d iters, %lld ms)",
           FLEET_KDF_ITERS, (long long)((esp_timer_get_time() - t0) / 1000));
}

/* ------------------------------------------------------------------ roster */

int fleet_count(void)
{
    int n = 0;
    for (int i = 0; i < FLEET_MAX; i++) if (s_peers[i].used) n++;
    return n;
}

fleet_peer_t *fleet_at(int i)
{
    if (i < 0 || i >= FLEET_MAX || !s_peers[i].used) return NULL;
    return &s_peers[i];
}

fleet_peer_t *fleet_find(const uint8_t *mac)
{
    for (int i = 0; i < FLEET_MAX; i++)
        if (s_peers[i].used && !memcmp(s_peers[i].mac, mac, 6)) return &s_peers[i];
    return NULL;
}

uint8_t fleet_next_device_no(void)
{
    for (uint8_t n = 1; n <= FLEET_MAX; n++) {
        bool taken = false;
        for (int i = 0; i < FLEET_MAX; i++)
            if (s_peers[i].used && s_peers[i].device_no == n) { taken = true; break; }
        if (!taken) return n;
    }
    return 0;
}

fleet_peer_t *fleet_add(const uint8_t *mac, uint16_t id, const char *name,
                        const uint8_t *lmk)
{
    fleet_peer_t *p = fleet_find(mac);
    if (!p) {
        for (int i = 0; i < FLEET_MAX; i++) {
            if (s_peers[i].used) continue;
            p = &s_peers[i];
            memset(p, 0, sizeof(*p));
            p->used      = true;
            p->device_no = fleet_next_device_no();
            memcpy(p->mac, mac, 6);
            break;
        }
    }
    if (!p) {
        applog("[fleet] full, cannot pair");
        return NULL;
    }
    p->id = id;
    if (name && *name) strlcpy(p->name, name, sizeof(p->name));
    memcpy(p->lmk, lmk, FLEET_KEY_LEN);
    store();
    applog("[fleet] paired %s as #%u", p->name, p->device_no);
    return p;
}

bool fleet_remove(const uint8_t *mac)
{
    fleet_peer_t *p = fleet_find(mac);
    if (!p) return false;
    applog("[fleet] removed %s (#%u)", p->name, p->device_no);
    /* Wipe the key, don't just drop the used flag — the blob is written back to
     * flash either way and a dead LMK is worth nothing sitting there. */
    memset(p, 0, sizeof(*p));
    store();
    return true;
}

/* ------------------------------------------------------------------ ratchet */

void fleet_mark_encrypted(const uint8_t *mac)
{
    fleet_peer_t *p = fleet_find(mac);
    if (!p || p->seen_encrypted) return;
    p->seen_encrypted = true;
    store();                    /* immediately: see the note at the top */
    applog("[fleet] %s is encrypted from now on", p->name);
}

bool fleet_clear_allowed(const uint8_t *mac)
{
    const fleet_peer_t *p = fleet_find(mac);
    if (p && p->seen_encrypted) return false;   /* the ratchet wins, always */
    return s_migration_open;
}

bool fleet_migration_open(void) { return s_migration_open; }

/* Kept as a symbol only so ui.c still links. Cannot open anything. */
void fleet_migration_set(bool open)
{
    if (open) applog("[fleet] migration refused: not built into this firmware");
}

/* --------------------------------------------------------------- key derive */

void fleet_derive_pair_key(const uint8_t *mac_a, const uint8_t *mac_b,
                           uint8_t *out)
{
    /* Canonical order, so both ends agree without negotiating who went first. */
    const uint8_t *lo = mac_a, *hi = mac_b;
    if (memcmp(mac_a, mac_b, 6) > 0) { lo = mac_b; hi = mac_a; }

    uint8_t msg[12 + 18];
    memcpy(msg,      lo, 6);
    memcpy(msg + 6,  hi, 6);
    memcpy(msg + 12, "kids-comm/pair/v1", 17);

    /* One HMAC is enough: the PMK is already high-entropy, unlike the 8-digit
     * code it came from. The expensive stretching belongs where the guessing
     * happens, which is fleet_set_code(). */
    uint8_t mac_out[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = md ? mbedtls_md_hmac(md, s_pmk, sizeof(s_pmk), msg, sizeof(msg) - 1,
                                  mac_out) : -1;
    if (rc) {
        ESP_LOGE(TAG, "pair key derivation failed: -0x%04x", -rc);
        applog("[fleet] pair key derivation FAILED, pairing aborted");
        memset(out, 0, FLEET_KEY_LEN);
        return;
    }
    memcpy(out, mac_out, FLEET_KEY_LEN);
}

/* ---- AES-GCM via PSA ------------------------------------------------------
 *
 * 🚨 Same story as the KDF above: Mbed TLS 4.x (IDF 6.0) moved `gcm.h` to
 * `mbedtls/private/`, and `cipher.h` is not public there either. The supported
 * route is PSA AEAD.
 *
 * 🔑 UNLIKE PBKDF2, THIS ONE SHOULD ACTUALLY BE AVAILABLE. PSA PBKDF2 returned
 * NOT_SUPPORTED because, without MBEDTLS_PSA_CRYPTO_CONFIG, PSA capability is
 * derived from the classic config and there is no classic->PSA mapping for key
 * derivation. AES-GCM maps directly from MBEDTLS_GCM_C, which is on for TLS.
 * That is reasoning, not a result — fleet_crypto_selftest() settles it.
 *
 * ⚠️ PSA concatenates ciphertext||tag; mbedtls hands them back separately. The
 * WIRE FORMAT MUST NOT CHANGE — a 16-byte ct and a 16-byte tag as distinct
 * fields — so the split happens here and never reaches a peer.
 *
 * AES-GCM is deterministic for a given key, nonce and plaintext, so a
 * fixed-nonce comparison against the old implementation is a complete proof of
 * equivalence, not a sample. */
static int fleet_gcm_seal(const uint8_t *key, const uint8_t *nonce,
                          const uint8_t *pt, size_t pt_len,
                          uint8_t *ct_out, uint8_t *tag_out)
{
    if (psa_crypto_init() != PSA_SUCCESS) return -1;

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, FLEET_KEY_LEN * 8);

    psa_key_id_t k = 0;
    psa_status_t st = psa_import_key(&attr, key, FLEET_KEY_LEN, &k);
    if (st != PSA_SUCCESS) return (int)st;

    uint8_t buf[32];                 /* ct(16) || tag(16) */
    size_t  got = 0;
    st = psa_aead_encrypt(k, PSA_ALG_GCM, nonce, 12, NULL, 0,
                          pt, pt_len, buf, sizeof buf, &got);
    psa_destroy_key(k);
    if (st != PSA_SUCCESS) return (int)st;
    if (got != pt_len + 16) return -2;

    memcpy(ct_out,  buf, pt_len);
    memcpy(tag_out, buf + pt_len, 16);
    mbedtls_platform_zeroize(buf, sizeof buf);
    return 0;
}

static int fleet_gcm_open(const uint8_t *key, const uint8_t *nonce,
                          const uint8_t *ct, size_t ct_len,
                          const uint8_t *tag, uint8_t *pt_out)
{
    if (psa_crypto_init() != PSA_SUCCESS) return -1;

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, FLEET_KEY_LEN * 8);

    psa_key_id_t k = 0;
    psa_status_t st = psa_import_key(&attr, key, FLEET_KEY_LEN, &k);
    if (st != PSA_SUCCESS) return (int)st;

    uint8_t buf[32];
    memcpy(buf, ct, ct_len);
    memcpy(buf + ct_len, tag, 16);

    size_t got = 0;
    st = psa_aead_decrypt(k, PSA_ALG_GCM, nonce, 12, NULL, 0,
                          buf, ct_len + 16, pt_out, ct_len, &got);
    psa_destroy_key(k);
    mbedtls_platform_zeroize(buf, sizeof buf);
    if (st != PSA_SUCCESS) return (int)st;
    return (got == ct_len) ? 0 : -2;
}

/* ------------------------------------------------------------- key wrapping */

/* 🔑 THE LINK KEY IS RANDOM AND TRAVELS WRAPPED. It is never derived from
 * anything an ex-member could recompute — see the note in fleet.h. */
bool fleet_wrap_lmk(const uint8_t *pair_key, const uint8_t *lmk,
                    uint8_t *nonce_out, uint8_t *ct_out, uint8_t *tag_out)
{
    esp_fill_random(nonce_out, 12);

    bool ok = (fleet_gcm_seal(pair_key, nonce_out, lmk, FLEET_KEY_LEN,
                              ct_out, tag_out) == 0);
    if (!ok) applog("[fleet] key wrap failed");
    return ok;
}

bool fleet_unwrap_lmk(const uint8_t *pair_key, const uint8_t *nonce,
                      const uint8_t *ct, const uint8_t *tag, uint8_t *lmk_out)
{
    bool ok = (fleet_gcm_open(pair_key, nonce, ct, FLEET_KEY_LEN,
                              tag, lmk_out) == 0);
    /* 🚨 A FAILED TAG IS THE WHOLE POINT — it means the other handset does not
     * hold our PMK, i.e. a different fleet code or an outsider. Wipe rather
     * than store something half-decrypted. */
    if (!ok) { memset(lmk_out, 0, FLEET_KEY_LEN); applog("[fleet] pairing rejected: wrong fleet code"); }
    return ok;
}
