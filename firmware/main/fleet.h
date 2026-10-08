#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Who is in the fleet, and the key for each link.
 *
 * 🔑 ESP-NOW KEYS ARE PER PEER, NOT PER FLEET. `lmk` is a field of
 * esp_now_peer_info_t; esp_now_set_pmk() is device-global but only encrypts the
 * LMK at rest. That is the whole reason this file exists rather than a single
 * #define'd fleet key:
 *
 *   - a shared fleet key CANNOT REVOKE. Dropping one handset means re-keying
 *     every remaining handset, which means re-flashing a fleet of children's
 *     devices to remove one of them.
 *   - with a key per link, adding a handset touches nobody else, and removing
 *     one is a delete on each of the others. No re-key, no re-flash.
 *
 * An outside research note (2026-09-05) recommended a shared fleet key. It was wrong
 * for what this fleet has to do; see COMMUNICATOR-SECURITY-PLAN.md.
 */

/* ⚠️ ESP_NOW_MAX_ENCRYPT_PEER_NUM in esp_now.h says 6 AND IS A RED HERRING —
 * it is referenced by nothing but its own header. The governing limit is
 * CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM (Kconfig range 0..17, default 7),
 * which sdkconfig.defaults sets to 10. Do not cut the fleet to 6 for it. */
#define FLEET_MAX       8       /* matches s_peers[8] in espnow.c */

/* 🔑 BUMP THIS TO WIPE EVERY STORED PAIRING ONCE, ON THE NEXT BOOT.
 *
 * Same idea as CAL_GEN in settings.c. Gen 2 clears the rosters left by the
 * 2026-09-06 send-race, where each link was paired twice and the two ends ended
 * up holding different keys while both displayed "locked". Twenty taps across
 * five handsets to unpick by hand; one flash this way.
 *
 * ⚠️ The PMK is deliberately KEPT — the fleet code does not need retyping, only
 * the broken link keys go. */
#define FLEET_GEN       2
#define FLEET_KEY_LEN   16      /* ESP_NOW_KEY_LEN */
#define FLEET_NAME_LEN  10      /* matches peer_t.name */

/* 🔑 EIGHT DIGITS, AND THE LENGTH IS A SECURITY PARAMETER, NOT A UX ONE.
 *
 * The pairing exchange is protected by a key derived from this code, so anyone
 * who RECORDS a pairing can take every possible code offline and try each one
 * until the exchange decrypts, recovering the link key. At 4 digits that is
 * 10,000 candidates — seconds of laptop time. At 8 it is 100 million, and
 * FLEET_KDF_ITERS makes each candidate cost real work.
 *
 * Typed once per handset by a parent. The builder chose 8 over 6 on 2026-09-06. */
#define FLEET_CODE_LEN  8

/* Slows an offline guess. One derivation happens per pairing, so a second of
 * handset time here is invisible to the user and expensive to an attacker.
 * ⚠️ The real cost on an S3 is logged by fleet_set_code() — check it rather
 * than trusting this comment. */
#define FLEET_KDF_ITERS 100000

typedef struct {
    bool     used;
    uint8_t  mac[6];
    uint16_t id;
    char     name[FLEET_NAME_LEN];
    uint8_t  lmk[FLEET_KEY_LEN];
    uint8_t  device_no;         /* 1..FLEET_MAX — the UI says "Player 3", not "XXXX" */

    /* 🔑 THE RATCHET, AND IT IS SET AT PAIRING, NOT FROM THE RECEIVE PATH.
     *
     * ⚠️ esp_now_recv_info_t carries src_addr, des_addr and rx_ctrl and NOTHING
     * THAT SAYS WHETHER THE FRAME WAS ENCRYPTED. The application cannot see it,
     * so "mark it once I hear an encrypted frame" is not implementable. What is
     * implementable: once this handset holds a key for a MAC we add that peer
     * with encrypt=true, and the stack decrypts with the LMK — a cleartext
     * frame from that MAC does not authenticate and never reaches on_recv.
     *
     * So enforcement lives BELOW us, which is stronger than an app-layer check
     * would have been. What this flag does is keep fleet_clear_allowed() from
     * ever handing that MAC back to the clear path while migration is open. */
    bool     seen_encrypted;
} fleet_peer_t;

void fleet_init(void);          /* load from NVS */

/* 🔎 THE EVIDENCE FOR THE KDF PORT - OFF IN EVERY SHIPPING BUILD.
 *
 * 1 compiles in fleet_kdf_selftest(), which derives a throwaway key with BOTH
 * `mbedtls_pkcs5_pbkdf2_hmac_ext()` and our own `fleet_pbkdf2()` and compares
 * the bytes. **Result on Parent, 2026-09-19: PASS, identical keys**
 * (pkcs5 14,265 ms vs ours 15,196 ms at FLEET_KDF_ITERS = 100,000.)
 *
 * 🚨 Two reasons it must stay 0:
 *   - it costs ~14 s of boot per derivation, and it does two;
 *   - it needs `mbedtls/pkcs5.h`, which IDF 6.0 makes PRIVATE, so a 6.0 build
 *     with this enabled will not compile. That is correct, not a bug: the
 *     point was to compare the old function against the new one while both
 *     still exist, and only 5.3 has both. */
#define FLEET_KDF_SELFTEST 0

#if FLEET_KDF_SELFTEST
void fleet_kdf_selftest(void);
#endif

/* 🚨 MEASURED 2026-09-06: THE PMK MUST BE THE SAME ON EVERY HANDSET.
 *
 * Two handsets with the same link LMK but different PMKs cannot pass a directed
 * message — the send fails with no MAC-layer ack. So the PMK is not merely
 * local key storage as the docs imply; it participates in the link. An earlier
 * draft of this file minted a RANDOM PMK PER HANDSET, which would have broken
 * every link in the fleet silently and looked exactly like a radio fault.
 *
 * It is therefore derived from the fleet code, which the parent types once on
 * each handset. ⚠️ Changing the fleet code re-keys the whole fleet. */
void fleet_set_code(const char *code);
bool fleet_has_pmk(void);
const uint8_t *fleet_pmk(void);

int           fleet_count(void);
fleet_peer_t *fleet_at(int i);              /* 0..FLEET_MAX-1, NULL if unused */
fleet_peer_t *fleet_find(const uint8_t *mac);
fleet_peer_t *fleet_add(const uint8_t *mac, uint16_t id, const char *name,
                        const uint8_t *lmk);
bool          fleet_remove(const uint8_t *mac);
void          fleet_save(void);

/* Set the ratchet. Called from the PAIRING path, once a key is stored — not
 * from on_recv, which cannot tell an encrypted frame from a clear one. */
void fleet_mark_encrypted(const uint8_t *mac);

/* THE decision, in one place: may we talk to this MAC in the clear?
 *
 * false if it has ever been seen encrypted (the ratchet), false if migration is
 * closed, true only for a handset that has never been paired while the parent
 * has deliberately opened migration. */
bool fleet_clear_allowed(const uint8_t *mac);

/* ⚠️ MIGRATION-ONLY: strip before public repo.
 *
 * Lets an encrypted handset keep talking to handsets that have not been flashed
 * yet, so a fleet in daily use can be upgraded a handset at a time instead of
 * all in one evening. It is a downgrade path and must not ship publicly. */
bool fleet_migration_open(void);
void fleet_migration_set(bool open);

uint8_t fleet_next_device_no(void);

/* Derive the key that protects a pairing exchange — NOT the link key itself.
 *
 * ⚠️ IT COMES FROM THE PMK, NOT THE FLEET CODE. The code is wiped the instant
 * it is accepted and is never stored, so pairing cannot ask for it later. The
 * PMK is derived from that code and both handsets already hold it, so it is the
 * same secret reached a different way — and nothing new lands in flash.
 *
 * 🚨 THE LINK LMK MUST BE RANDOM, NEVER DERIVED FROM THE CODE. A derived key is
 * only as private as its inputs, and the MACs are broadcast in the clear by the
 * beacon: a removed handset that kept the fleet code could recompute
 * PBKDF2(code, A||B) — the key for a link it was never part of — and read
 * traffic between two handsets that both dropped it. Revocation would look like
 * it worked and would not.
 *
 * So: this derives a short-lived key from the code; the random LMK is generated
 * at pairing and exchanged under it.
 *
 * ⚠️ The salt is both MACs in canonical order, so both ends agree without
 * negotiating who went first, and the same code used by a different pair gives
 * a different key. */
void fleet_derive_pair_key(const uint8_t *mac_a, const uint8_t *mac_b,
                           uint8_t *out /* FLEET_KEY_LEN */);

/* Wrap/unwrap a random link key for the exchange, AES-GCM.
 *
 * 🔑 AES-GCM AND NOT ChaCha20-Poly1305, WHICH AN OUTSIDE RESEARCH NOTE PREFERRED.
 * CONFIG_MBEDTLS_CHACHA20_C is off in this build and GCM is already on, the S3
 * accelerates AES in hardware, and the overhead is identical (12-byte nonce +
 * 16-byte tag). Enabling ChaCha would cost flash for no gain here.
 *
 * Returns false on any failure — a caller must NEVER fall through to an
 * unwrapped or zero key. */
bool fleet_wrap_lmk(const uint8_t *pair_key, const uint8_t *lmk,
                    uint8_t *nonce_out /* 12 */, uint8_t *ct_out /* 16 */,
                    uint8_t *tag_out /* 16 */);
bool fleet_unwrap_lmk(const uint8_t *pair_key, const uint8_t *nonce,
                      const uint8_t *ct, const uint8_t *tag,
                      uint8_t *lmk_out /* FLEET_KEY_LEN */);
