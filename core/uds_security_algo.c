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
 *   s_level_keys[] below contains PLACEHOLDER KEYS only.
 *   These are NOT secret and must be replaced before production deployment.
 *
 *   Required actions before vehicle deployment:
 *     1. Do NOT commit real OEM keys to source control.
 *     2. Inject keys at runtime via uds_security_algo_set_level_key()
 *        from a secure source (OTP fuses, HSM, secure boot chain), OR
 *        replace s_level_keys[] in a secure build environment, OR
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
 *
 * SINGLE SECURITY CONTEXT PER PROCESS:
 *   All key material and derivation state (s_level_keys[], s_sequence,
 *   s_rng_cb, s_derive_cb, s_lfsr, s_placeholder_keys[]) is held in
 *   module-static globals. This module is NOT re-entrant or instantiable:
 *   there is exactly one active security context per process/image, by
 *   design, matching the one-ECU-personality-per-image deployment model.
 *   A process that tries to run two independent security contexts in the
 *   same image will have the second silently share (and can clobber) the
 *   first's keys/sequence state. See docs/ARCHITECTURE.md's Security
 *   Manager section.
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
 * Production: must be set to n in prj.conf.  When a build declares itself
 * production (see EDS_BUILD_IS_PRODUCTION in uds_security_algo.h), this
 * #error fires, preventing a production firmware image from being linked
 * at all while s_level_keys[] still holds the compile-time placeholder
 * values.
 *
 * WHAT THIS CHECK ACTUALLY DOES (corrected — see issue #84 PR discussion):
 * this is NOT byte-pattern detection of s_level_keys[]'s contents. A C
 * preprocessor #if cannot inspect a runtime array initializer's byte
 * values, and no mechanism anywhere in this codebase extracts those bytes
 * into a preprocessor-visible macro. The gate is an unconditional compile
 * failure whenever a production build is declared: it forces the
 * integrator to affirmatively prove intent — inject real keys via
 * uds_security_algo_set_level_key() at runtime, or edit s_level_keys[]
 * directly in a secure build environment — and then flip the build-mode
 * signal to production, before CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=n (or
 * the FreeRTOS/override equivalent) can be believed. The actual runtime
 * check of whether real keys were injected is
 * uds_security_algo_keys_are_placeholder(), called from the generated
 * Step 7.1 init guard (see tools/templates/uds_init.c.j2) — this #error
 * does not duplicate that check, it just refuses to build at all until
 * the integrator has gone through the motions of declaring production.
 * If an OEM's integration process needs to keep placeholder keys in a
 * build that is otherwise production in every other respect, the gate can
 * be silenced by keeping CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=y (or
 * EDS_BUILD_IS_PRODUCTION=0) in that build only — with a deviation record
 * filed per your ASIL-B change control process.
 *
 * BUILD-MODE DERIVATION: see EDS_BUILD_IS_PRODUCTION in
 * uds_security_algo.h (SEC-BUILD-MODE-01) for the full four-case
 * derivation, including the Zephyr autoconf.h omission quirk that made
 * the OLD version of this gate (`defined(X) && !X`) never fire on a real
 * Zephyr production build. This gate no longer re-derives THAT logic —
 * it consumes the shared primitive so the two cannot drift apart again.
 *
 * WHY THIS SITE ALSO KEEPS ITS OWN `!defined(UNIT_TEST)` ON TOP OF
 * EDS_BUILD_IS_PRODUCTION: EDS_BUILD_IS_PRODUCTION answers "is this a
 * production build" in general — it does not, and should not, also try to
 * answer CRIT-4's narrower question: "does this specific compiled binary
 * actually contain real, non-placeholder OEM keys." A host unit-test
 * binary (build_tests.sh, build_harness.sh, the CMake test path) can
 * deliberately force EDS_BUILD_IS_PRODUCTION=1 — via
 * -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 — purely to exercise a gate's
 * PRODUCTION *behaviour* (see test_trng_fail_closed.c, which forces this
 * exact combination to reach ALGO_ENTROPY_FAIL_CLOSED's fail-closed
 * path). That binary is not pretending to be a deployable image and never
 * has real keys injected via OTP/HSM, so CRIT-4 firing there would be
 * asserting something true but useless: it would make it permanently
 * impossible to compile ANY host test that forces the production path for
 * anything else in this file. `!defined(UNIT_TEST)` is this gate's own,
 * narrow, CRIT-4-specific exemption for exactly that case — it is NOT a
 * copy of the #84 bug and must not be folded into EDS_BUILD_IS_PRODUCTION
 * itself (that would silently exempt every consumer of the shared macro,
 * including ALGO_ENTROPY_FAIL_CLOSED, defeating the point of forcing the
 * production path from a host test at all). UNIT_TEST is exclusively a
 * host-test-build macro — it is never defined by any real Zephyr Kconfig,
 * any FreeRTOS CMakeLists.txt, or any bare-metal toolchain in this
 * codebase — so for every real firmware image (Zephyr or FreeRTOS, dev or
 * production) `!defined(UNIT_TEST)` is always true and this condition
 * reduces to exactly EDS_BUILD_IS_PRODUCTION, i.e. the #84 fix is intact
 * for every build that matters.
 *
 * TRACEABILITY: SEC-KEY-GATE-01 / CRIT-4
 * ============================================================================= */

#if EDS_BUILD_IS_PRODUCTION && !defined(UNIT_TEST)
#error "[SEC-KEY-GATE-01] Production build declared (EDS_BUILD_IS_PRODUCTION=1) "\
       "but placeholder keys are still present in uds_security_algo.c. "\
       "Inject real OEM keys via uds_security_algo_set_level_key() before "\
       "building production firmware, then set CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=n "\
       "in your production prj.conf (or the FreeRTOS/EDS_BUILD_IS_PRODUCTION "\
       "equivalent for non-Zephyr builds)."
#endif

/* =============================================================================
 * [SEC-TRNG-FAILCLOSED-01] Entropy fail-closed build-mode gate
 *
 * PROBLEM: when no TRNG callback is registered (or the registered TRNG
 * callback fails at runtime), algo_get_random() historically fell back to
 * a deterministic software LFSR and SecurityAccess seeds kept being issued.
 * A security subsystem must fail CLOSED on loss of its entropy source, not
 * silently degrade to a predictable PRNG. ALGO_ENTROPY_FAIL_CLOSED controls
 * whether algo_get_random() is permitted to use the LFSR fallback:
 *   1 -> fail closed: LFSR is NEVER used; entropy failure is refused.
 *   0 -> LFSR fallback permitted (development / CI builds only).
 *
 * This must be a PRODUCTION-ONLY behaviour change: development and CI
 * builds keep today's LFSR fallback so existing host/dev workflows are
 * unaffected; only real production firmware fails closed.
 *
 * [#84 UPDATE] ALGO_ENTROPY_FAIL_CLOSED is now a plain alias for
 * EDS_BUILD_IS_PRODUCTION (see uds_security_algo.h, SEC-BUILD-MODE-01) —
 * it no longer derives "is this production?" itself. This file used to
 * carry an independent copy of the same four-case Zephyr autoconf.h /
 * UNIT_TEST / omitted-symbol derivation that the CRIT-4 gate below carried
 * too, and the two had drifted: the CRIT-4 gate used the older,
 * broken `defined(X) && !X` idiom (never fired on a real Zephyr
 * production build — the symbol is omitted, not defined-and-zero, when
 * the Kconfig bool is `n`), while this gate already used the corrected
 * "positive signal for development, fail closed by default" idiom. Two
 * independently-authored copies of the same yes/no answer are exactly how
 * that drift happened; unifying them into one primitive removes the
 * ability for it to recur. The alias keeps the ALGO_ENTROPY_FAIL_CLOSED
 * name — it is already used throughout this file's comments,
 * test_trng_fail_closed.c, and SECURITY.md, and renaming it everywhere
 * would be unrelated churn to already-shipped, already-reviewed code.
 *
 * RETROACTIVE FIX FOR FREERTOS: because EDS_BUILD_IS_PRODUCTION's default
 * (omitted Kconfig symbol, no UNIT_TEST) is PRODUCTION, and FreeRTOS
 * builds have never defined CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY, a real
 * FreeRTOS build has always resolved ALGO_ENTROPY_FAIL_CLOSED to 1
 * (fail-closed) with no supported way to opt into the LFSR dev fallback —
 * a latent, CI-invisible gap in the already-merged PR #85 (FreeRTOS CI
 * jobs are build-only; they never exercise a live SecurityAccess request).
 * Each FreeRTOS example's CMakeLists.txt now defines
 * CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY itself (default ON, i.e. development —
 * mirroring Zephyr Kconfig's own `default y`), giving FreeRTOS integrators
 * the same supported opt-in Zephyr already had. See SEC-BUILD-MODE-01 for
 * the full four-case derivation this alias now inherits.
 *
 * ALGO_ENTROPY_FAIL_CLOSED may also be forced directly on the compiler
 * command line (-DALGO_ENTROPY_FAIL_CLOSED=<0|1>). The `#if !defined(...)`
 * guard immediately below is what makes that override take effect: it
 * skips the alias `#define` entirely when the command line already
 * defined ALGO_ENTROPY_FAIL_CLOSED, so the literal command-line value
 * stands on its own rather than being re-aliased to EDS_BUILD_IS_PRODUCTION.
 * This is a SEPARATE override from -DEDS_BUILD_IS_PRODUCTION=<0|1>: forcing
 * one does not force the other unless both are passed — set
 * -DEDS_BUILD_IS_PRODUCTION=1 instead (or as well) if the intent is to also
 * force the CRIT-4 gate at the same time.
 *
 * TRACEABILITY: SEC-TRNG-FAILCLOSED-01 (derives from SEC-BUILD-MODE-01)
 * ============================================================================= */
#if !defined(ALGO_ENTROPY_FAIL_CLOSED)
#define ALGO_ENTROPY_FAIL_CLOSED EDS_BUILD_IS_PRODUCTION
#endif

/* --------------------------------------------------------------------------
 * Internal constants
 * -------------------------------------------------------------------------- */

/** AES-128 key size used for level keys. */
#define ALGO_AES_KEY_LEN      (16U)

/** Maximum number of security levels supported. */
#define ALGO_MAX_LEVELS       (2U)

/** Number of level keys defined in s_level_keys[]. */
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
 *
 * [HIGH-2 FIX / SEC-TRNG-FAILCLOSED-01] Behaviour depends on
 * ALGO_ENTROPY_FAIL_CLOSED (see the build-mode gate near the top of this
 * file). There are three entry scenarios:
 *
 *   1. No TRNG callback registered (s_rng_cb == NULL):
 *        - Development builds (ALGO_ENTROPY_FAIL_CLOSED == 0): unchanged —
 *          fall through to the LFSR. s_trng_fallback_count is NOT touched:
 *          that counter is documented as counting TRNG *call* failures, and
 *          a callback that was never registered never made a call.
 *        - Production builds (ALGO_ENTROPY_FAIL_CLOSED == 1): refuse. Record
 *          a platform violation and return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE
 *          without ever running the LFSR.
 *
 *   2. TRNG callback registered and returns UDS_STATUS_OK: unchanged in both
 *      modes — the hardware entropy is used and the LFSR never runs.
 *
 *   3. TRNG callback registered but returns non-OK (runtime hardware fault):
 *      s_trng_fallback_count is ALWAYS incremented (saturating) — this is a
 *      genuine TRNG call failure in both modes.
 *        - Development builds: unchanged — record UDS_STATUS_ERR_PLATFORM as
 *          a platform violation (soft degraded fallback) and fall through to
 *          the LFSR for graceful degradation.
 *        - Production builds: record UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE as
 *          the platform violation (hard refusal), scrub the caller's buffer,
 *          and return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE. The LFSR does NOT
 *          run.
 *
 * DISTINGUISHING THE TWO FAILURE MODES IN THE SAFETY CONTEXT:
 * Both the development soft-fallback and the production hard-refusal bump
 * the SAME uds_safety platform_violations counter (the correct bucket per
 * the safety rules — a TRNG fault is a platform event, not a protocol
 * violation) — no new counter or field is added to uds_safety; this is a
 * deliberate reuse of the existing HIGH-2 pattern. They are distinguished
 * by last_violation_code instead: the soft degraded fallback records
 * UDS_STATUS_ERR_PLATFORM (0x60), while the hard production refusal records
 * UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE (0x24).
 *
 * @return UDS_STATUS_OK if buf was filled (TRNG or, in development builds,
 *         LFSR fallback).
 * @return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE in production builds when no
 *         entropy source is available (no callback registered, or the
 *         registered callback failed). buf is left scrubbed to zero in the
 *         registered-but-failed case; untouched in the no-callback case
 *         (caller has not written to it yet at this point in the flow).
 *
 * TRACEABILITY: SEC-TRNG-FAULT-01 / HIGH-2, SEC-TRNG-FAILCLOSED-01
 */
static uds_status_t algo_get_random(uint8_t *buf, uint8_t len)
{
    uint8_t i;

    if (s_rng_cb == NULL) {
#if ALGO_ENTROPY_FAIL_CLOSED
        /*
         * [SEC-TRNG-FAILCLOSED-01] Production build, no TRNG registered.
         * Fail closed: do NOT run the LFSR. s_trng_fallback_count is a
         * call-failure counter and is deliberately not touched here — no
         * call was ever made.
         */
        uds_safety_record_platform_violation(UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE);
        return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE;
#endif
        /* Development/CI build: fall through to the LFSR below. */
    } else if (s_rng_cb(buf, len) == UDS_STATUS_OK) {
        return UDS_STATUS_OK;
    } else {
        /*
         * [HIGH-2 FIX] TRNG callback was registered but failed at runtime.
         *
         * This is a mid-session hardware degradation event: the entropy source
         * was present at startup (passed the Step 7.1 production gate) but has
         * since become unreliable. s_trng_fallback_count is incremented in
         * BOTH build modes — it counts raw TRNG call failures, and that is
         * true regardless of what happens next.
         *
         * TRACEABILITY: SEC-TRNG-FAULT-01 / HIGH-2
         */
        if (s_trng_fallback_count < UINT32_MAX) {
            s_trng_fallback_count++;
        }
#if ALGO_ENTROPY_FAIL_CLOSED
        /*
         * [SEC-TRNG-FAILCLOSED-01] Production build: hard refusal. Record
         * the SEED_UNAVAILABLE code (distinct from the dev-mode PLATFORM
         * code below) as last_violation_code, scrub the caller's buffer so
         * no partial/stale data can leak out, and refuse — the LFSR must
         * NOT run.
         */
        uds_safety_record_platform_violation(UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE);
        (void)memset(buf, 0, (size_t)len);
        return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE;
#else
        /*
         * Development/CI build: unchanged from before this change. Record
         * the persistent, field-accessible platform-violation counter and
         * fall through to the LFSR for graceful degradation. The seed
         * generated will be of degraded quality — the counters record this
         * so auditors and field engineers know it happened.
         */
        uds_safety_record_platform_violation(UDS_STATUS_ERR_PLATFORM);
        /* Fall through to LFSR for graceful degradation. */
#endif
    }

    /* Software LFSR fallback (development only — see counters above). */
    for (i = (uint8_t)0U; i < len; i += (uint8_t)2U) {
        uint16_t rnd = algo_lfsr_next();
        buf[i] = (uint8_t)(rnd & (uint16_t)0xFFU);
        if ((i + (uint8_t)1U) < len) {
            buf[i + (uint8_t)1U] = (uint8_t)((rnd >> 8U) & (uint16_t)0xFFU);
        }
    }
    return UDS_STATUS_OK;
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

    /*
     * [SEC-TRNG-FAILCLOSED-01] Generate entropy FIRST, and only advance/
     * commit the sequence counter after entropy succeeded. A refused seed
     * request (production build, no usable TRNG) must not burn sequence
     * numbers — this only changes behaviour on the error path, which was
     * unreachable before this change (algo_get_random() previously could
     * not fail).
     */
    {
        uds_status_t rng_status = algo_get_random(nonce, (uint8_t)UDS_ALGO_SEED_NONCE_LEN);
        if (rng_status != UDS_STATUS_OK) {
            (void)memset(nonce, 0, sizeof(nonce));
            return rng_status;
        }
    }

    /* Advance monotonic sequence counter; skip 0x0000 (reserved sentinel). */
    s_sequence++;
    if (s_sequence == (uint16_t)0U) {
        s_sequence = (uint16_t)1U;
    }

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
