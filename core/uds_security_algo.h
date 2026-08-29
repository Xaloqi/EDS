// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_security_algo.h
 *
 * PURPOSE: Production seed/key algorithm for UDS SecurityAccess (SID 0x27).
 *
 * PHASE 1 — Production Security Hardening [P1-SEC]
 *   Replaces the Phase 5 XOR reference stub with AES-128-CMAC + TRNG.
 *
 * ─────────────────────────────────────────────────────────────────────────
 * ALGORITHM OVERVIEW
 * ─────────────────────────────────────────────────────────────────────────
 *
 *   SEED GENERATION
 *   ───────────────
 *   The 8-byte seed is structured as:
 *     Byte 0..5 : 48-bit TRNG nonce (from hardware RNG)
 *     Byte 6    : security_level (embedded for domain separation)
 *     Byte 7    : sequence_lo    (lower 8 bits of monotonic counter)
 *
 *   KEY DERIVATION
 *   ─────────────────────────────────────────
 *   expected_key = TRUNCATE(AES-128-CMAC(level_key, seed), UDS_ALGO_KEY_LEN)
 *
 *   Where:
 *     level_key  = 128-bit per-level AES key (in protected memory / OTP)
 *     seed       = the 8-byte seed sent to the tester
 *     TRUNCATE   = keep first UDS_ALGO_KEY_LEN (4) bytes of the 16-byte MAC
 *
 * ─────────────────────────────────────────────────────────────────────────
 * OEM PLUGGABLE INTERFACE
 * ─────────────────────────────────────────────────────────────────────────
 *
 *   1. TRNG CALLBACK:
 *        uds_security_algo_set_rng_cb()  — register hardware entropy source
 *
 *   2. FULL ALGORITHM OVERRIDE:
 *        uds_security_algo_set_derive_cb() — replaces AES-CMAC entirely
 *        Use for HSM offload, proprietary algorithms, AUTOSAR Csm, etc.
 *
 *   3. PER-LEVEL KEY INJECTION:
 *        uds_security_algo_set_level_key() — inject 128-bit key from OTP/HSM
 *        REQUIRED before production deployment.
 *        Compile-time keys are PLACEHOLDERS ONLY.
 *
 * ─────────────────────────────────────────────────────────────────────────
 * WIRING
 * ─────────────────────────────────────────────────────────────────────────
 *   Register TRNG + inject keys before first session:
 *     uds_security_algo_set_rng_cb(my_trng_cb);
 *     uds_security_algo_set_level_key(0x01U, otp_key_level1);
 *     uds_security_algo_set_level_key(0x03U, otp_key_level2);
 *
 *   Pass callbacks to security module (unchanged from Phase 5):
 *     cfg.seed_generate_cb = uds_security_algo_generate_seed;
 *     cfg.key_validate_cb  = uds_security_algo_validate_key;
 *
 * LEVEL SUPPORT:
 *   Level 1 (0x01/0x02) — entry access
 *   Level 2 (0x03/0x04) — elevated access
 *
 * THREAD SAFETY: Not thread-safe. Call from UDS task context only.
 *
 * SAFETY  : ASIL-B candidate. OEM must perform safety assessment.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef UDS_SECURITY_ALGO_H
#define UDS_SECURITY_ALGO_H

#include "uds_types.h"
#include <stdint.h>
#include <stdbool.h>

/* =============================================================================
 * [SEC-BUILD-MODE-01] Shared build-mode primitive: EDS_BUILD_IS_PRODUCTION
 *
 * PROBLEM (issue #84): this codebase had TWO independently-authored copies
 * of "is this a production build?" — the CRIT-4 placeholder-key #error in
 * uds_security_algo.c, and the Step 7.1 runtime guard generated into every
 * examples/*\/generated/uds_init.c from tools/templates/uds_init.c.j2 — and
 * they drifted to the SAME wrong answer independently:
 *
 *   #if defined(CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY) && \
 *       !CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY
 *
 * CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY is a Kconfig bool. Zephyr's generated
 * autoconf.h emits `#define CONFIG_X 1` for a bool set to `y`, and OMITS
 * the symbol entirely for `n` — it never emits `#define CONFIG_X 0`. So on
 * a real Zephyr PRODUCTION build (the bool set to `n`, exactly what the
 * docs tell integrators to do), defined(CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY)
 * is FALSE, and `defined(X) && !X` is false too — both gates silently did
 * nothing in precisely the configuration they exist to protect.
 *
 * FIX: derive "is this production?" from ONE macro, defined here, and have
 * every gate in this codebase (this file's own ALGO_ENTROPY_FAIL_CLOSED
 * alias, the CRIT-4 #error in uds_security_algo.c, and the Step 7.1 guard
 * in the uds_init.c.j2 template) reference it instead of re-deriving the
 * same logic a second (or third) time. One definition cannot drift from
 * itself; that is the actual root-cause fix for #84, not just a polarity
 * flip on the two existing call sites.
 *
 * FOUR CASES, in priority order:
 *
 *   1. CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY is #defined (any value):
 *      Kconfig bool was resolved to `y` by Zephyr's autoconf.h (value 1),
 *      or a build explicitly forced it to a literal 0 or 1 on the command
 *      line (e.g. -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0, the convention
 *      test_trng_fail_closed.c and build_tests.sh already use to reach the
 *      production path from a host build; FreeRTOS's CMakeLists.txt now
 *      does this too, unconditionally, via a $<BOOL:...> generator
 *      expression — see examples/*_freertos/CMakeLists.txt). Non-zero ->
 *      development/CI (placeholder keys explicitly permitted) -> NOT
 *      production. Zero -> production.
 *   2. Symbol undefined, but UNIT_TEST is defined:
 *      Host unit-test / harness build (build_tests.sh, build_harness.sh).
 *      No autoconf.h exists at all here and no build system defines the
 *      Kconfig symbol. Treated as development so existing host suites keep
 *      today's behaviour. NOTE: this case only applies when
 *      CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY is undefined — case 1 takes
 *      priority, so a host test that defines the symbol itself (to reach
 *      the production path deliberately) is honoured over UNIT_TEST.
 *   3. Neither symbol is defined:
 *      Zephyr with the Kconfig bool set to `n` (symbol omitted from
 *      autoconf.h — the case the old buggy idiom missed), FreeRTOS,
 *      bare metal, or any platform not yet ported. PRODUCTION. This is
 *      the default: the safe state is what you get by omission, on every
 *      platform, including ones EDS does not ship a port for yet.
 *
 *      FreeRTOS has no Kconfig equivalent of its own, so without its build
 *      system explicitly defining CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY it
 *      would land here on EVERY build, including CI. Case 3 is why
 *      examples/*_freertos/CMakeLists.txt now defines the symbol itself
 *      (defaulting to development, mirroring Zephyr Kconfig's own
 *      `default y`) — see SEC-KEY-GATE-01 and that CMakeLists.txt for the
 *      mechanism. A FreeRTOS build that has NOT picked up that fix still
 *      correctly falls through to this case and fails closed, rather than
 *      silently doing nothing the way the pre-#84 gates did.
 *
 * ESCAPE HATCH: EDS_BUILD_IS_PRODUCTION may be forced directly on the
 * compiler command line (-DEDS_BUILD_IS_PRODUCTION=<0|1>), overriding all
 * of the above. This is the supported override for a build whose signals
 * disagree with its actual intent (see the CRIT-4 deviation process
 * documented at that gate for one example).
 *
 * HOST TEST NOTE: a host test that wants to force the production path
 * compiles with -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 (defined AND zero).
 * That hits case 1 above (the nested nonzero/zero check), not the
 * omitted-symbol path of case 3 — but lands on the same production value,
 * so it is a faithful stand-in for real Zephyr/FreeRTOS production
 * hardware without needing a cross-compiled toolchain in a host test.
 *
 * NOT EVERY CONSUMER SHOULD STOP HERE: this macro answers ONE question —
 * "is this a production build" — in general. A specific gate may be
 * asking a narrower question that "production, in general" does not fully
 * capture, and may need its own additional qualifier layered on top of
 * EDS_BUILD_IS_PRODUCTION rather than folded into it. Worked example: the
 * CRIT-4 placeholder-key #error in uds_security_algo.c is not just asking
 * "is this production" — it's asking "does this compiled binary actually
 * contain real OEM keys," which a host unit-test build forcing production
 * *behaviour* (to exercise some other gate's logic, e.g.
 * ALGO_ENTROPY_FAIL_CLOSED via test_trng_fail_closed.c) can never
 * satisfy. CRIT-4 therefore guards itself with its own, additional
 * `&& !defined(UNIT_TEST)` at its call site — see that gate's comment for
 * the full reasoning. That qualifier belongs at CRIT-4's call site only,
 * NOT here: baking it into EDS_BUILD_IS_PRODUCTION itself would silently
 * exempt every consumer of this macro from ever seeing "production" while
 * UNIT_TEST is defined, including ALGO_ENTROPY_FAIL_CLOSED — which would
 * break the very host-test forcing mechanism this note just described.
 * Add narrowing conditions at the consumer, never inside this primitive.
 *
 * TRACEABILITY: SEC-BUILD-MODE-01 (root cause of issue #84). Referenced by
 * SEC-KEY-GATE-01 (CRIT-4, uds_security_algo.c) and SEC-TRNG-FAILCLOSED-01
 * (ALGO_ENTROPY_FAIL_CLOSED, uds_security_algo.c) — neither redefines this
 * logic; both derive from it.
 * ============================================================================= */
#if !defined(EDS_BUILD_IS_PRODUCTION)
#  if defined(CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY)
#    if CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY
#      define EDS_BUILD_IS_PRODUCTION (0)
#    else
#      define EDS_BUILD_IS_PRODUCTION (1)
#    endif
#  elif defined(UNIT_TEST)
#    define EDS_BUILD_IS_PRODUCTION (0)
#  else
#    define EDS_BUILD_IS_PRODUCTION (1)
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Seed / key size constants
 * -------------------------------------------------------------------------- */

/**
 * @brief Total seed length in bytes.
 *
 * Layout:
 *   Byte 0..5 : TRNG nonce (48 bits)
 *   Byte 6    : security_level (domain separator)
 *   Byte 7    : sequence_lo (anti-replay counter, lower 8 bits)
 */
#define UDS_ALGO_SEED_LEN              (8U)

/**
 * @brief UDS key response length in bytes.
 * First UDS_ALGO_KEY_LEN bytes of the 16-byte AES-CMAC output.
 */
#define UDS_ALGO_KEY_LEN               (4U)

/** Byte offset of the TRNG nonce field in the seed. */
#define UDS_ALGO_SEED_NONCE_OFFSET     (0U)

/** Length of the TRNG nonce field in bytes. */
#define UDS_ALGO_SEED_NONCE_LEN        (6U)

/** Byte offset of the sequence counter HIGH byte in the seed.
 * [P1-SEC] Security level is NOT embedded in seed bytes to allow 2-byte sequence.
 * Domain separation is achieved via the per-level AES key, not a seed byte. */
#define UDS_ALGO_SEED_SEQ_HI_OFFSET    (6U)

/** Byte offset of the sequence counter field (LOW byte) in the seed. */
#define UDS_ALGO_SEED_SEQ_OFFSET       (7U)

/** Backwards-compat alias: SEQ_HI byte is at old LEVEL_OFFSET position. */
#define UDS_ALGO_SEED_LEVEL_OFFSET     UDS_ALGO_SEED_SEQ_HI_OFFSET

/* --------------------------------------------------------------------------
 * Callback types
 * -------------------------------------------------------------------------- */

/**
 * @brief Platform TRNG callback — provides hardware random bytes.
 *
 * @param[out] buf  Output buffer.
 * @param[in]  len  Number of bytes requested.
 * @return UDS_STATUS_OK on success, UDS_STATUS_ERR_PLATFORM on failure.
 */
typedef uds_status_t (*uds_algo_rng_cb_t)(uint8_t *buf, uint8_t len);

/**
 * @brief OEM full key-derivation override callback.
 *
 * When registered, replaces the built-in AES-128-CMAC for all levels.
 *
 * @param[in]  security_level  UDS security level (odd or even sub-function).
 * @param[in]  seed            Seed bytes (UDS_ALGO_SEED_LEN bytes).
 * @param[out] key_out         Derived key output (UDS_ALGO_KEY_LEN bytes).
 * @return UDS_STATUS_OK on success.
 */
typedef uds_status_t (*uds_algo_derive_cb_t)(
    uint8_t        security_level,
    const uint8_t *seed,
    uint8_t       *key_out);

/* --------------------------------------------------------------------------
 * Module configuration
 * -------------------------------------------------------------------------- */

/**
 * @brief Register a hardware TRNG source.
 *
 * MUST be called with a real entropy source before production deployment.
 *
 * Passing NULL reverts to the software LFSR fallback ONLY in development/CI
 * builds (ALGO_ENTROPY_FAIL_CLOSED == 0 — see uds_security_algo.c). In a
 * production build (Zephyr build with CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=n),
 * passing NULL — or leaving no callback registered — instead makes every
 * subsequent uds_security_algo_generate_seed() call fail closed and return
 * UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE; the LFSR is never used to satisfy a
 * production seed request.
 *
 * @param[in] rng_cb  TRNG callback. May be NULL.
 *
 * TRACEABILITY: SEC-TRNG-FAILCLOSED-01
 */
void uds_security_algo_set_rng_cb(uds_algo_rng_cb_t rng_cb);

/**
 * @brief [HIGH-2 FIX] Return the currently registered TRNG callback.
 *
 * Returns NULL if no TRNG has been registered.
 * Used by the generated init guard (Step 7.1) to enforce TRNG presence
 * in production builds (CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=n).
 *
 * TRACEABILITY: SEC-TRNG-GATE-01 / HIGH-2
 */
uds_algo_rng_cb_t uds_security_algo_get_rng_cb(void);

/**
 * @brief Register an OEM key-derivation override.
 *
 * Replaces built-in AES-CMAC when set. Pass NULL to use built-in (default).
 *
 * @param[in] derive_cb  OEM derivation callback. May be NULL.
 */
void uds_security_algo_set_derive_cb(uds_algo_derive_cb_t derive_cb);

/**
 * @brief Inject a 128-bit AES key for a security level.
 *
 * Call during secure boot to replace compile-time placeholder keys.
 * Keys are copied internally from key_128bit.
 *
 * @param[in] security_level  Odd seed sub-function (0x01, 0x03).
 *                            Even key sub-functions are also accepted.
 * @param[in] key_128bit      16-byte AES-128 key.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if key_128bit is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if level maps to no slot.
 */
uds_status_t uds_security_algo_set_level_key(
    uint8_t        security_level,
    const uint8_t *key_128bit);

/**
 * @brief Reset module to power-on state (test use only).
 */
void uds_security_algo_reset(void);

/**
 * @brief Return current sequence counter (for test validation).
 */
uint16_t uds_security_algo_get_sequence(void);

/**
 * @brief [HIGH-2 FIX] Return the number of TRNG call failures observed.
 *
 * Counts how many times s_rng_cb was registered and non-NULL but returned
 * a non-OK status during seed generation — i.e. raw TRNG *call* failures.
 * A callback that was never registered does NOT count here: see
 * uds_security_algo_set_rng_cb() for that case.
 *
 * What happens after each counted failure differs by build mode
 * (ALGO_ENTROPY_FAIL_CLOSED — see uds_security_algo.c):
 *   - Development/CI builds: each counted failure is followed by a
 *     software LFSR fallback so the caller still receives a (degraded)
 *     seed and UDS_STATUS_OK.
 *   - Production builds: each counted failure is followed by a hard
 *     refusal — uds_security_algo_generate_seed() returns
 *     UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE and the LFSR is never used.
 * In both modes the failure also increments uds_safety platform_violations,
 * which persists across session transitions and is readable via a DID; the
 * two failure modes are distinguished there by last_violation_code
 * (UDS_STATUS_ERR_PLATFORM for the dev-mode soft fallback vs.
 * UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE for the production hard refusal).
 *
 * A non-zero value indicates that the hardware entropy source has degraded
 * at least once since the last power cycle or uds_security_algo_reset() call.
 *
 * Typical usage: register an application hook that polls this counter after
 * each seed generation and triggers a DTC or refuses further SecurityAccess
 * requests if the count exceeds a threshold (e.g. 3 consecutive failures).
 *
 * Counter is reset to 0 by uds_security_algo_reset() only.
 * It is NOT reset by session transitions (intentional — mirrors the safety
 * module counter behaviour).
 *
 * @return Number of TRNG call failures since last reset.
 *         Returns 0 when no TRNG is registered (no calls have been made).
 *
 * TRACEABILITY: SEC-TRNG-FAULT-01 / HIGH-2, SEC-TRNG-FAILCLOSED-01
 */
uint32_t uds_security_algo_get_trng_fallback_count(void);

/**
 * @brief [CRIT-4 FIX] Runtime check: are any key slots still placeholder?
 *
 * Returns true if at least one security level still holds the factory-default
 * placeholder key (0x00..0x0F or 0x10..0x1F).  Returns false only after
 * uds_security_algo_set_level_key() has been called for ALL defined levels.
 *
 * Called by the generated init sequence (Step 7.1) to abort startup when
 * CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=n but real keys have not been injected.
 * Also useful for runtime diagnostics / DID reporting.
 *
 * @return true  at least one key slot is still a placeholder.
 * @return false all key slots replaced via uds_security_algo_set_level_key().
 *
 * TRACEABILITY: SEC-KEY-GATE-01 / CRIT-4
 */
bool uds_security_algo_keys_are_placeholder(void);

/* --------------------------------------------------------------------------
 * Seed generation (uds_security_seed_generate_fn compatible)
 * -------------------------------------------------------------------------- */

/**
 * @brief Generate a seed for UDS SecurityAccess.
 *
 * @param[in]  security_level  UDS seed sub-function (odd: 0x01, 0x03).
 * @param[out] seed_buf        Output buffer.
 * @param[in]  seed_buf_len    Buffer size (>= UDS_ALGO_SEED_LEN).
 * @param[out] out_seed_len    Bytes written (always UDS_ALGO_SEED_LEN).
 *
 * [SEC-TRNG-FAILCLOSED-01] Entropy availability differs by build mode
 * (ALGO_ENTROPY_FAIL_CLOSED — see uds_security_algo.c):
 *   - Development/CI builds: if no TRNG is registered, or the registered
 *     TRNG callback fails, a software LFSR fallback is used and this
 *     function still returns UDS_STATUS_OK (unchanged legacy behaviour).
 *   - Production builds (Zephyr build with
 *     CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=n): the LFSR is never used to
 *     satisfy a seed request. If no TRNG is registered, or the registered
 *     TRNG callback fails, this function fails closed: it returns
 *     UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE, seed_buf and *out_seed_len are
 *     left untouched, and the internal sequence counter is NOT advanced
 *     (a refused request must not burn a sequence number).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if buffer too small or level unknown.
 * @return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE (production builds only) if no
 *         entropy source is available. See core/uds_security.c, which maps
 *         this to the same code from the seed_generate_cb contract, and
 *         core/uds_server.c, which maps it to NRC 0x24
 *         (requestSequenceError).
 */
uds_status_t uds_security_algo_generate_seed(
    uint8_t  security_level,
    uint8_t *seed_buf,
    uint8_t  seed_buf_len,
    uint8_t *out_seed_len);

/* --------------------------------------------------------------------------
 * Key validation (uds_security_key_validate_fn compatible)
 * -------------------------------------------------------------------------- */

/**
 * @brief Validate a key received from the tester.
 *
 * Computes expected = TRUNCATE(AES-128-CMAC(level_key, seed), KEY_LEN)
 * and compares with received key using constant-time comparison.
 * Also validates anti-replay sequence counter.
 *
 * @param[in] security_level  UDS key sub-function (even: 0x02, 0x04).
 * @param[in] seed            Seed previously sent to tester.
 * @param[in] seed_len        Seed length in bytes.
 * @param[in] key             Key received from tester.
 * @param[in] key_len         Key length in bytes.
 *
 * @return true if key is valid.
 * @return false on any failure (wrong key, replay, unknown level, NULL).
 */
bool uds_security_algo_validate_key(
    uint8_t        security_level,
    const uint8_t *seed,
    uint8_t        seed_len,
    const uint8_t *key,
    uint8_t        key_len);

/* --------------------------------------------------------------------------
 * Key derivation helper (tester-side tools and unit tests)
 * -------------------------------------------------------------------------- */

/**
 * @brief Derive the expected key from seed and security level.
 *
 * No replay check — intended for tester implementation and test use.
 *
 * @param[in]  security_level  UDS security level (odd or even).
 * @param[in]  seed            Seed bytes (UDS_ALGO_SEED_LEN bytes).
 * @param[out] key_out         Derived key (UDS_ALGO_KEY_LEN bytes).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if level unknown.
 */
uds_status_t uds_security_algo_derive_key(
    uint8_t        security_level,
    const uint8_t *seed,
    uint8_t       *key_out);

#ifdef __cplusplus
}
#endif

#endif /* UDS_SECURITY_ALGO_H */
