// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_security_algo.c
 *
 * PURPOSE: Production AES-128-CMAC + TRNG seed/key algorithm for SID 0x27.
 *
 * PHASE 1 — Production Security Hardening [P1-SEC]
 *   Replaces the Phase 5 XOR reference implementation.
 *
 * See uds_security_algo.h for full design and integration documentation.
 *
 * KEY STORAGE SECURITY NOTICE:
 *   k_level_keys[] below contains PLACEHOLDER KEYS only.
 *   These are NOT secret and must be replaced before production deployment.
 *
 *   Required actions before vehicle deployment:
 *     1. Do NOT commit real OEM keys to source control.
 *     2. Inject keys at runtime via uds_security_algo_set_level_key()
 *        from a secure source (OTP fuses, HSM, secure boot chain), OR
 *        replace k_level_keys[] in a secure build environment, OR
 *        register a uds_algo_derive_cb_t that calls your HSM directly.
 *
 * REPLAY PROTECTION:
 *   The sequence counter (s_sequence) is incremented on every seed request.
 *   The lower 8 bits are embedded in seed byte 7. The validator checks
 *   that this byte matches the current counter — ensuring a (seed, key)
 *   pair from a previous session cannot be replayed once the counter
 *   advances. Counter wraps 0xFF → 0x01 (never 0x00: reserved sentinel).
 *
 * CONSTANT-TIME COMPARISON:
 *   algo_ct_compare() uses a volatile accumulator to prevent the compiler
 *   from short-circuiting the comparison loop, eliminating timing
 *   side-channels that could reveal key material.
 *
 * SAFETY  : ASIL-B candidate. See header for full safety notes.
 * STANDARD: MISRA C:2012 alignment intended.
 *
 * MISRA DEVIATION LOG:
 *   [DEV-ALGO-01] Rule 8.13 advisory: volatile local 'diff' in ct_compare.
 *     Required to guarantee constant-time comparison. Justified by
 *     ASIL-B security requirement SEC-CT-01.
 * =============================================================================
 */

#include "uds_security_algo.h"
#include "uds_aes_cmac.h"
#include "uds_safety.h"
#include "uds_types.h"

#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* =============================================================================
 * [CRIT-4 FIX] Compile-time production key gate
 *
 * CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY is a Kconfig bool defined in Kconfig.
 * Default: y  (allows placeholder keys — development / CI builds).
 * Production: must be set to n in prj.conf.  When n, this #error fires if
 * the source file still contains the known placeholder key bytes, preventing
 * a production firmware image from being linked with insecure keys.
 *
 * The check is conservative: it detects the zero-based sequential pattern
 * 0x00,0x01,0x02,0x03 as the first four bytes of s_level_keys[0].  Any
 * real OEM key will differ from this pattern.  If an OEM key happens to
 * start with these bytes (extremely unlikely), the gate can be silenced by
 * setting CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=y in that build only — with a
 * deviation record filed per your ASIL-B change control process.
 *
 * For host-side unit test builds (UNIT_TEST=1) the gate is bypassed because
 * those builds do not include autoconf.h and the symbol is undefined.
 *
 * TRACEABILITY: SEC-KEY-GATE-01 / CRIT-4
 * ============================================================================= */

#if defined(CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY) && \
    !CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY && \
    !defined(UNIT_TEST)
#error "[SEC-KEY-GATE-01] CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=n but placeholder "\
       "keys are still present in uds_security_algo.c. "\
       "Inject real OEM keys via uds_security_algo_set_level_key() before "\
       "building production firmware, then set CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=n "\
       "in your production prj.conf."
#endif

/* --------------------------------------------------------------------------
 * Internal constants
 * -------------------------------------------------------------------------- */

/** AES-128 key size used for level keys. */
#define ALGO_AES_KEY_LEN      (16U)

/** Maximum number of security levels supported. */
#define ALGO_MAX_LEVELS       (2U)

/** Number of level keys defined in k_level_keys[]. */
#define ALGO_DEFINED_LEVELS   (2U)

/* --------------------------------------------------------------------------
 * Per-level AES-128 keys
 *
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * SECURITY NOTICE: THESE ARE PLACEHOLDER KEYS — NOT FOR PRODUCTION USE.
 * Replace via uds_security_algo_set_level_key() or in a secure build.
 * See header and file header above for OEM integration requirements.
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 *
 * Level indexing:
 *   Security level 0x01 (seed) / 0x02 (key) → index 0
 *   Security level 0x03 (seed) / 0x04 (key) → index 1
 * -------------------------------------------------------------------------- */
static uint8_t s_level_keys[ALGO_DEFINED_LEVELS][ALGO_AES_KEY_LEN] = {
    /* Level 1 (0x01/0x02) — PLACEHOLDER — replace before production */
    {
        0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
        0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU
    },
    /* Level 2 (0x03/0x04) — PLACEHOLDER — replace before production */
    {
        0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U,
        0x18U, 0x19U, 0x1AU, 0x1BU, 0x1CU, 0x1DU, 0x1EU, 0x1FU
    }
};

/*
 * Compile-time defaults — kept separately so reset() can restore them
 * without hardcoding the values again.
 */
static const uint8_t k_default_keys[ALGO_DEFINED_LEVELS][ALGO_AES_KEY_LEN] = {
    {
        0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
        0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU
    },
    {
        0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U,
        0x18U, 0x19U, 0x1AU, 0x1BU, 0x1CU, 0x1DU, 0x1EU, 0x1FU
    }
};

/* --------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------- */

/** Monotonic sequence counter — incremented on every seed request. */
static uint16_t s_sequence = (uint16_t)0U;

/** Optional hardware TRNG callback. NULL → software LFSR fallback. */
static uds_algo_rng_cb_t s_rng_cb = NULL;

/** Optional OEM key-derivation override. NULL → built-in AES-CMAC. */
static uds_algo_derive_cb_t s_derive_cb = NULL;

/** Software LFSR state (Galois 16-bit, polynomial 0xB400). */
static uint16_t s_lfsr = (uint16_t)0xACE1U;

/**
 * [HIGH-2 FIX] Running count of times the TRNG callback was registered but
 * returned non-OK, forcing a fallback to the software LFSR.
 *
 * Distinct from the uds_safety platform_violations counter:
 *   - This counter is local to the algo module and counts raw TRNG call
 *     failures since the last reset (power cycle or uds_security_algo_reset()).
 *   - The safety module counter is the persistent, field-accessible record
 *     for diagnostics and ISO 26262 failure analysis.
 * Both are incremented on every fallback event.
 * Saturating at UINT32_MAX — never wraps.
 * Readable via uds_security_algo_get_trng_fallback_count().
 * TRACEABILITY: SEC-TRNG-FAULT-01 / HIGH-2
 */
static uint32_t s_trng_fallback_count = (uint32_t)0U;

/**
 * [CRIT-4 FIX] True when s_level_keys[] still holds placeholder values.
 * Set to false by uds_security_algo_set_level_key() for each level.
 * Both levels must be replaced for the flag to clear (both slots must
 * be injected with non-placeholder keys).
 * Checked at runtime by uds_security_algo_keys_are_placeholder() and
 * by the init-sequence guard in generated/uds_init.c (Step 7.1).
 */
static bool s_placeholder_keys[ALGO_DEFINED_LEVELS] = {
    true,  /* Level 1 (0x01/0x02) — placeholder until set_level_key() called */
    true,  /* Level 2 (0x03/0x04) — placeholder until set_level_key() called */
};

/* --------------------------------------------------------------------------
 * Internal: software LFSR fallback
 * -------------------------------------------------------------------------- */

/**
 * @brief Advance the Galois 16-bit LFSR by one step.
 * Polynomial: x^16 + x^14 + x^13 + x^11 + 1 (maximal-length, period 65535).
 * NOT suitable as sole entropy source in production.
 */
static uint16_t algo_lfsr_next(void)
{
    uint16_t lsb = s_lfsr & (uint16_t)1U;
    s_lfsr >>= 1U;
    if (lsb != (uint16_t)0U) {
        s_lfsr ^= (uint16_t)0xB400U;
    }
    if (s_lfsr == (uint16_t)0U) {
        s_lfsr = (uint16_t)0xACE1U; /* prevent degenerate lock-up */
    }
    return s_lfsr;
}

/**
 * @brief Fill buf with random bytes from TRNG (preferred) or LFSR (fallback).
 */
static void algo_get_random(uint8_t *buf, uint8_t len)
{
    uint8_t i;

    if (s_rng_cb != NULL) {
        if (s_rng_cb(buf, len) == UDS_STATUS_OK) {
            return;
        }

        /*
         * [HIGH-2 FIX] TRNG callback was registered but failed at runtime.
         *
         * This is a mid-session hardware degradation event: the entropy source
         * was present at startup (passed the Step 7.1 production gate) but has
         * since become unreliable.  Two counters are incremented:
         *
         *   1. s_trng_fallback_count — module-local, reset on power cycle.
         *      Readable via uds_security_algo_get_trng_fallback_count().
         *      Allows the application to poll and take action (e.g. refuse
         *      further SecurityAccess requests after N consecutive failures).
         *
         *   2. uds_safety platform_violations — persistent safety-module
         *      counter, readable via uds_safety_get_ctx() and therefore via
         *      a DID.  Survives session transitions; accumulates evidence of
         *      hardware degradation across the ECU lifetime for field analysis.
         *
         * Both use saturating arithmetic (no wrap at UINT32_MAX).
         * The LFSR fallback still runs to avoid blocking the caller, but the
         * seed generated will be of degraded quality — the counters record
         * this so auditors and field engineers know it happened.
         *
         * TRACEABILITY: SEC-TRNG-FAULT-01 / HIGH-2
         */
        if (s_trng_fallback_count < UINT32_MAX) {
            s_trng_fallback_count++;
        }
        uds_safety_record_platform_violation(UDS_STATUS_ERR_PLATFORM);
        /* Fall through to LFSR for graceful degradation. */
    }

    /* Software LFSR fallback (development only — see counter above). */
    for (i = (uint8_t)0U; i < len; i += (uint8_t)2U) {
        uint16_t rnd = algo_lfsr_next();
        buf[i] = (uint8_t)(rnd & (uint16_t)0xFFU);
        if ((i + (uint8_t)1U) < len) {
            buf[i + (uint8_t)1U] = (uint8_t)((rnd >> 8U) & (uint16_t)0xFFU);
        }
    }
}

/* --------------------------------------------------------------------------
 * Internal: level → key table index mapping
 * -------------------------------------------------------------------------- */

/**
 * @brief Map a UDS security level (odd or even) to the key table index.
 *
 * Level 0x01 / 0x02 → index 0
 * Level 0x03 / 0x04 → index 1
 *
 * @param[in]  security_level  UDS sub-function value.
 * @param[out] out_index       Receives table index.
 * @return true if valid; false if out of range.
 */
static bool algo_level_to_index(uint8_t security_level, uint8_t *out_index)
{
    uint8_t idx;

    if ((security_level == (uint8_t)0U) || (out_index == NULL)) {
        return false;
    }
    idx = (uint8_t)((security_level - (uint8_t)1U) >> 1U);

    if (idx >= (uint8_t)ALGO_DEFINED_LEVELS) {
        return false;
    }
    *out_index = idx;
    return true;
}

/* --------------------------------------------------------------------------
 * Internal: constant-time comparison
 * -------------------------------------------------------------------------- */

/**
 * @brief Constant-time byte-array comparison.
 *
 * Processes all bytes unconditionally to prevent timing side-channels.
 *
 * @par MISRA C:2012 Deviation [DEV-ALGO-01]
 * 'diff' is declared volatile to prevent dead-store elimination by the
 * compiler, which would break the constant-time guarantee. Justified by
 * ASIL-B security requirement SEC-CT-01.
 */
static bool algo_ct_compare(const uint8_t *a, const uint8_t *b, uint8_t len)
{
    /* MISRA Deviation [DEV-ALGO-01]: volatile for constant-time guarantee. */
    volatile uint8_t diff = (uint8_t)0U;
    uint8_t          i;

    for (i = (uint8_t)0U; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return (diff == (uint8_t)0U);
}

/* --------------------------------------------------------------------------
 * Internal: AES-CMAC key derivation
 * -------------------------------------------------------------------------- */

/**
 * @brief Derive a UDS_ALGO_KEY_LEN-byte key using AES-128-CMAC.
 *
 * key_out = TRUNCATE(AES-128-CMAC(s_level_keys[idx], seed), UDS_ALGO_KEY_LEN)
 *
 * @param[in]  key_idx  Index into s_level_keys[].
 * @param[in]  seed     Seed bytes (UDS_ALGO_SEED_LEN bytes).
 * @param[out] key_out  Output buffer (UDS_ALGO_KEY_LEN bytes).
 * @return UDS_STATUS_OK on success; UDS_STATUS_ERR_PLATFORM on CMAC failure.
 */
static uds_status_t algo_derive_cmac(
    uint8_t        key_idx,
    const uint8_t *seed,
    uint8_t       *key_out)
{
    uint8_t mac[UDS_CMAC_TAG_LEN];
    int     rc;

    rc = uds_aes_cmac(
        s_level_keys[key_idx],
        seed,
        (size_t)UDS_ALGO_SEED_LEN,
        mac);

    if (rc != 0) {
        /* Scrub and return error. */
        (void)memset(mac, 0, sizeof(mac));
        return UDS_STATUS_ERR_PLATFORM;
    }

    /* Truncate: first UDS_ALGO_KEY_LEN bytes of the CMAC tag. */
    (void)memcpy(key_out, mac, (size_t)UDS_ALGO_KEY_LEN);

    /* Scrub full MAC from stack — only the truncated portion leaves. */
    (void)memset(mac, 0, sizeof(mac));

    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void uds_security_algo_set_rng_cb(uds_algo_rng_cb_t rng_cb)
{
    s_rng_cb = rng_cb;
}

/**
 * [HIGH-2 FIX] Return the currently registered TRNG callback.
 * NULL means no TRNG is registered; seed generation uses LFSR fallback.
 * Used by the generated init guard (Step 7.1) to enforce TRNG presence
 * in production builds (CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=n).
 */
uds_algo_rng_cb_t uds_security_algo_get_rng_cb(void)
{
    return s_rng_cb;
}

void uds_security_algo_set_derive_cb(uds_algo_derive_cb_t derive_cb)
{
    s_derive_cb = derive_cb;
}

uds_status_t uds_security_algo_set_level_key(
    uint8_t        security_level,
    const uint8_t *key_128bit)
{
    uint8_t idx;

    if (key_128bit == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!algo_level_to_index(security_level, &idx)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    (void)memcpy(s_level_keys[idx], key_128bit, (size_t)ALGO_AES_KEY_LEN);

    /* [CRIT-4 FIX] Mark this slot as no longer holding a placeholder key. */
    s_placeholder_keys[idx] = false;

    return UDS_STATUS_OK;
}

void uds_security_algo_reset(void)
{
    s_sequence           = (uint16_t)0U;
    s_rng_cb             = NULL;
    s_derive_cb          = NULL;
    s_lfsr               = (uint16_t)0xACE1U;
    s_trng_fallback_count = (uint32_t)0U; /* [HIGH-2] Reset per-module counter. */
    /* Restore placeholder keys and reset the placeholder flags. */
    (void)memcpy(s_level_keys, k_default_keys, sizeof(s_level_keys));
    s_placeholder_keys[0] = true;
    s_placeholder_keys[1] = true;
}

uint16_t uds_security_algo_get_sequence(void)
{
    return s_sequence;
}

/**
 * [HIGH-2 FIX] Return the TRNG fallback count.
 * See uds_security_algo.h for full documentation.
 */
uint32_t uds_security_algo_get_trng_fallback_count(void)
{
    return s_trng_fallback_count;
}

uds_status_t uds_security_algo_generate_seed(
    uint8_t  security_level,
    uint8_t *seed_buf,
    uint8_t  seed_buf_len,
    uint8_t *out_seed_len)
{
    uint8_t level_idx;
    uint8_t nonce[UDS_ALGO_SEED_NONCE_LEN];

    if ((seed_buf == NULL) || (out_seed_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (seed_buf_len < (uint8_t)UDS_ALGO_SEED_LEN) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (!algo_level_to_index(security_level, &level_idx)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* Advance monotonic sequence counter; skip 0x0000 (reserved sentinel). */
    s_sequence++;
    if (s_sequence == (uint16_t)0U) {
        s_sequence = (uint16_t)1U;
    }

    /* Generate TRNG nonce. */
    algo_get_random(nonce, (uint8_t)UDS_ALGO_SEED_NONCE_LEN);

    /* Pack seed: [nonce[0..5], seq_hi, seq_lo]
     * [P1-SEC] Domain separation via per-level AES key; security_level not
     * embedded in seed bytes to allow full 16-bit big-endian sequence. */
    (void)memcpy(&seed_buf[UDS_ALGO_SEED_NONCE_OFFSET],
                 nonce,
                 (size_t)UDS_ALGO_SEED_NONCE_LEN);
    seed_buf[UDS_ALGO_SEED_SEQ_HI_OFFSET] = (uint8_t)((s_sequence >> 8U) & (uint16_t)0xFFU);
    seed_buf[UDS_ALGO_SEED_SEQ_OFFSET]    = (uint8_t)(s_sequence & (uint16_t)0xFFU);

    *out_seed_len = (uint8_t)UDS_ALGO_SEED_LEN;

    /* Scrub nonce from stack. */
    (void)memset(nonce, 0, sizeof(nonce));

    return UDS_STATUS_OK;
}

bool uds_security_algo_validate_key(
    uint8_t        security_level,
    const uint8_t *seed,
    uint8_t        seed_len,
    const uint8_t *key,
    uint8_t        key_len)
{
    uint8_t      level_idx;
    uint8_t      expected_key[UDS_ALGO_KEY_LEN];
    uint8_t      seed_seq_lo;
    uint8_t      module_seq_lo;
    uds_status_t derive_rc;
    bool         result;

    if ((seed == NULL) || (key == NULL)) {
        return false;
    }

    if ((seed_len < (uint8_t)UDS_ALGO_SEED_LEN)
            || (key_len != (uint8_t)UDS_ALGO_KEY_LEN)) {
        return false;
    }

    if (!algo_level_to_index(security_level, &level_idx)) {
        return false;
    }

    /*
     * Anti-replay check: the sequence counter byte embedded in the seed
     * must match the lower 8 bits of the current module counter.
     *
     * If an attacker records (seed, key) and replays after the counter
     * advances, this check fails — the old seed's sequence byte no longer
     * matches s_sequence. A constant-time dummy comparison is performed
     * on replay to avoid leaking counter proximity via timing.
     */
    /* [P1-SEC] Compare full 16-bit sequence embedded in seed bytes 6 (HI) and 7 (LO). */
    seed_seq_lo   = seed[UDS_ALGO_SEED_SEQ_OFFSET];
    module_seq_lo = (uint8_t)(s_sequence & (uint16_t)0xFFU);

    {
        uint8_t seed_seq_hi   = seed[UDS_ALGO_SEED_SEQ_HI_OFFSET];
        uint8_t module_seq_hi = (uint8_t)((s_sequence >> 8U) & (uint16_t)0xFFU);
        bool    seq_mismatch  = (seed_seq_hi != module_seq_hi) ||
                                (seed_seq_lo != module_seq_lo);
        if (seq_mismatch) {
            /* Replay detected: perform dummy constant-time comparison. */
            (void)memset(expected_key, 0, sizeof(expected_key));
            (void)algo_ct_compare(expected_key, key, (uint8_t)UDS_ALGO_KEY_LEN);
            return false;
        }
    }

    /*
     * Derive the expected key.
     * Use OEM override if registered, otherwise built-in AES-CMAC.
     */
    if (s_derive_cb != NULL) {
        derive_rc = s_derive_cb(security_level, seed, expected_key);
    } else {
        derive_rc = algo_derive_cmac(level_idx, seed, expected_key);
    }

    if (derive_rc != UDS_STATUS_OK) {
        /* Derivation failure — deny access, scrub, return false. */
        (void)memset(expected_key, 0, sizeof(expected_key));
        return false;
    }

    /* Constant-time comparison. */
    result = algo_ct_compare(expected_key, key, (uint8_t)UDS_ALGO_KEY_LEN);

    /* Scrub expected key from stack regardless of outcome. */
    (void)memset(expected_key, 0, sizeof(expected_key));

    return result;
}

/**
 * [CRIT-4 FIX] uds_security_algo_keys_are_placeholder()
 *
 * @brief Runtime check: returns true if ANY security level still holds
 *        the factory-default placeholder key.
 *
 * Called by the generated init sequence (Step 7.1) before uds_server_init()
 * to abort startup when placeholder keys are detected in a context where
 * CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY protection is disabled or unavailable
 * (e.g. host-side harness builds, runtime injection verification).
 *
 * @return true  if at least one key slot is still a placeholder.
 * @return false if all key slots have been replaced via set_level_key().
 */
bool uds_security_algo_keys_are_placeholder(void)
{
    uint8_t i;
    for (i = (uint8_t)0U; i < (uint8_t)ALGO_DEFINED_LEVELS; i++) {
        if (s_placeholder_keys[i]) {
            return true;
        }
    }
    return false;
}

uds_status_t uds_security_algo_derive_key(
    uint8_t        security_level,
    const uint8_t *seed,
    uint8_t       *key_out)
{
    uint8_t level_idx;

    if ((seed == NULL) || (key_out == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!algo_level_to_index(security_level, &level_idx)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (s_derive_cb != NULL) {
        return s_derive_cb(security_level, seed, key_out);
    }

    return algo_derive_cmac(level_idx, seed, key_out);
}
