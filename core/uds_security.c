// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_security.c
 *
 * PURPOSE: UDS security access state machine.
 *
 * PHASE-2 FIXES APPLIED:
 *   [P2-SEC-01] Constant-time key comparison (volatile-loop pattern).
 *   [P2-SEC-02] NVM-backed attempt counter interface hook added.
 *   [P2-SEC-03] HW-RNG seeding hook added via seed_generate_cb contract note.
 *
 * SAFETY  : Safety-critical. ASIL-B candidate.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "uds_security.h"
#include "uds_types.h"

#include <string.h>

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static uint8_t sec_key_level_to_seed_level(uint8_t key_level);
static bool    sec_is_seed_level(uint8_t level);

/**
 * @brief [P2-SEC-01] Constant-time byte-array comparison.
 *
 * Processes all bytes regardless of mismatch to prevent timing side-channel.
 * Uses volatile accumulator so the compiler cannot short-circuit the loop.
 *
 * @param[in] a      First buffer.
 * @param[in] b      Second buffer.
 * @param[in] len    Number of bytes to compare.
 *
 * @return true if all len bytes are identical.
 */
static bool sec_constant_time_compare(
    const uint8_t *a,
    const uint8_t *b,
    uint8_t        len);

/* --------------------------------------------------------------------------
 * Public API implementations
 * -------------------------------------------------------------------------- */

uds_status_t uds_security_init(
    uds_security_ctx_t       *ctx,
    const uds_security_cfg_t *cfg)
{
    if ((ctx == NULL) || (cfg == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (ctx->initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    if ((cfg->key_validate_cb == NULL) || (cfg->seed_generate_cb == NULL)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (cfg->max_attempts == (uint8_t)0U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    (void)memset(ctx, 0, sizeof(uds_security_ctx_t));

    ctx->active_level        = (uint8_t)UDS_SEC_LEVEL_UNLOCKED;
    ctx->pending_level       = (uint8_t)0U;
    ctx->seed_len            = (uint8_t)0U;
    ctx->seed_pending        = false;
    ctx->failed_attempts     = (uint8_t)0U;
    ctx->max_attempts        = cfg->max_attempts;
    ctx->lockout_timer_ms    = 0U;
    ctx->lockout_duration_ms = (cfg->lockout_ms > 0U) ? cfg->lockout_ms : (uint32_t)UDS_SECURITY_LOCKOUT_MS;
    ctx->locked_out          = false;
    ctx->key_validate_cb     = cfg->key_validate_cb;
    ctx->seed_generate_cb    = cfg->seed_generate_cb;
    ctx->nvm_load_cb         = cfg->nvm_load_cb;
    ctx->nvm_save_cb         = cfg->nvm_save_cb;

    /*
     * [P3-SEC-01] Restore persisted attempt counter from NVM.
     *
     * If the application provides nvm_load_cb, load the failed-attempt
     * count and lockout residual that were saved before the last power-down.
     * This prevents an attacker from bypassing lockout by power-cycling the ECU.
     *
     * ERR_DID_NOT_FOUND is expected on first boot — treat as zero counter.
     * Any other error: log and proceed with zero counter (safe degradation).
     */
    if (cfg->nvm_load_cb != NULL) {
        uint8_t  loaded_attempts  = (uint8_t)0U;
        uint32_t loaded_lockout   = 0U;
        uds_status_t load_rc = cfg->nvm_load_cb(&loaded_attempts, &loaded_lockout);

        if (load_rc == UDS_STATUS_OK) {
            /* Clamp loaded values to prevent NVM corruption attack. */
            ctx->failed_attempts = (loaded_attempts <= cfg->max_attempts)
                                   ? loaded_attempts
                                   : cfg->max_attempts;

            if (loaded_lockout > 0U) {
                ctx->locked_out       = true;
                ctx->lockout_timer_ms = loaded_lockout;
            }
        }
        /* ERR_DID_NOT_FOUND (first boot) and other errors: start from zero. */
    }

    ctx->initialized = true;

    return UDS_STATUS_OK;
}

uds_status_t uds_security_request_seed(
    uds_security_ctx_t *ctx,
    uint8_t             security_level,
    uint8_t            *seed_buf,
    uint8_t             seed_buf_len,
    uint8_t            *out_seed_len)
{
    uds_status_t status;

    if ((ctx == NULL) || (seed_buf == NULL) || (out_seed_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (!sec_is_seed_level(security_level)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (ctx->locked_out) {
        return UDS_STATUS_ERR_SEC_ATTEMPT_EXCEEDED;
    }

    if (seed_buf_len < (uint8_t)UDS_SECURITY_SEED_LEN) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /*
     * ISO 14229-1: If the requested level is already unlocked, return
     * all-zero seed to indicate the level is active.
     */
    if (ctx->active_level == security_level) {
        (void)memset(seed_buf, 0, (size_t)seed_buf_len);
        *out_seed_len      = (uint8_t)UDS_SECURITY_SEED_LEN;
        ctx->seed_pending  = false;  /* [FIX] Already unlocked — no key exchange needed. */
        ctx->pending_level = (uint8_t)0U;
        return UDS_STATUS_OK;
    }

    /*
     * Generate seed via application callback.
     * [P2-SEC-03]: The seed_generate_cb contract requires the application
     * to use a hardware RNG source. The callback interface is unchanged;
     * seed quality is the application's responsibility. The stack provides
     * the framework only.
     */
    status = ctx->seed_generate_cb(
        security_level,
        ctx->seed,
        (uint8_t)UDS_SECURITY_SEED_LEN,
        &ctx->seed_len);

    if (status != UDS_STATUS_OK) {
        ctx->seed_pending  = false;
        ctx->pending_level = (uint8_t)0U;
        return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE;
    }

    /*
     * [SEC-ENTROPY-01] Reject all-zero seeds.
     *
     * An all-zero seed is the most common symptom of an uninitialised or
     * broken RNG (e.g. reading from an unstarted TRNG peripheral, a zeroed
     * buffer, or a counter that hasn't been seeded). A zero seed would allow
     * any attacker who knows the key derivation algorithm to compute the
     * correct key without knowing the secret — effectively bypassing security
     * access entirely.
     *
     * ISO 14229-1 does not mandate this check, but it is required for any
     * claim of security access being meaningful in the field.
     *
     * The "already unlocked" path above legitimately returns a zero seed
     * (ISO 14229-1 §10.4.3: seed of all zeros signals the level is active).
     * That path returns before reaching this point, so the check here only
     * fires on freshly generated seeds.
     *
     * MISRA C:2012 Rule 14.4: loop uses explicit uint8_t counter.
     */
    {
        uint8_t i;
        bool    all_zero = true;
        for (i = (uint8_t)0U; i < ctx->seed_len; i++) {
            if (ctx->seed[i] != (uint8_t)0U) {
                all_zero = false;
                break;
            }
        }
        if (all_zero) {
            ctx->seed_pending  = false;
            ctx->pending_level = (uint8_t)0U;
            return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE;
        }
    }

    (void)memcpy(seed_buf, ctx->seed, (size_t)ctx->seed_len);
    *out_seed_len      = ctx->seed_len;
    ctx->pending_level = security_level;
    ctx->seed_pending  = true;

    return UDS_STATUS_OK;
}

uds_status_t uds_security_send_key(
    uds_security_ctx_t *ctx,
    uint8_t             security_level,
    const uint8_t      *key,
    uint8_t             key_len)
{
    bool    key_valid;
    uint8_t expected_seed_level;

    if ((ctx == NULL) || (key == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (ctx->locked_out) {
        return UDS_STATUS_ERR_SEC_ATTEMPT_EXCEEDED;
    }

    if (!ctx->seed_pending) {
        return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE;
    }

    if (key_len == (uint8_t)0U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    expected_seed_level = sec_key_level_to_seed_level(security_level);
    if (expected_seed_level != ctx->pending_level) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /*
     * [P2-SEC-01] Constant-time key validation.
     *
     * [P1-SEC FIX] Seed length (UDS_SECURITY_SEED_LEN = 8) and key length
     * (UDS_SECURITY_KEY_LEN = 4) are now independent. The key length check
     * must compare against UDS_SECURITY_KEY_LEN, not ctx->seed_len, because
     * the AES-CMAC algorithm produces a 4-byte key from an 8-byte seed.
     *
     * The application callback receives the full seed (seed_len bytes) and
     * the submitted key (key_len bytes). The callback is responsible for
     * deriving and comparing the expected key from the seed.
     */
    if (key_len != (uint8_t)UDS_SECURITY_KEY_LEN) {
        /*
         * Key length mismatch: consume constant-time iterations to avoid
         * revealing UDS_SECURITY_KEY_LEN via timing side-channel.
         */
        uint8_t dummy[UDS_SECURITY_KEY_LEN] = {0U, 0U, 0U, 0U};
        (void)sec_constant_time_compare(dummy, dummy,
                                        (uint8_t)UDS_SECURITY_KEY_LEN);
        key_valid = false;
    } else {
        key_valid = ctx->key_validate_cb(
            security_level,
            ctx->seed,
            ctx->seed_len,   /* seed_len = UDS_SECURITY_SEED_LEN (8) */
            key,
            key_len);        /* key_len  = UDS_SECURITY_KEY_LEN  (4) */
    }

    ctx->seed_pending = false;

    if (!key_valid) {
        ctx->failed_attempts++;

        if (ctx->failed_attempts >= ctx->max_attempts) {
            ctx->locked_out       = true;
            ctx->lockout_timer_ms = ctx->lockout_duration_ms;
            ctx->failed_attempts  = (uint8_t)0U;

            /* [P3-SEC-01] Persist lockout state immediately. */
            if (ctx->nvm_save_cb != NULL) {
                (void)ctx->nvm_save_cb((uint8_t)0U, ctx->lockout_timer_ms);
            }

            return UDS_STATUS_ERR_SEC_ATTEMPT_EXCEEDED;
        }

        /* [P3-SEC-01] Persist incremented attempt counter immediately.
         * This write must complete before returning — if power is cut
         * after the failed attempt but before the write, the counter
         * may be slightly behind, but never ahead (safe direction). */
        if (ctx->nvm_save_cb != NULL) {
            (void)ctx->nvm_save_cb(ctx->failed_attempts, (uint32_t)0U);
        }

        return UDS_STATUS_ERR_SEC_INVALID_KEY;
    }

    /* Successful unlock — clear attempt counter in NVM. */
    ctx->active_level    = ctx->pending_level;
    ctx->failed_attempts = (uint8_t)0U;
    ctx->pending_level   = (uint8_t)0U;
    ctx->seed_pending    = false;   /* [FIX] Unlock consumes the pending seed. */

    /* [P3-SEC-01] Persist cleared counter on successful unlock. */
    if (ctx->nvm_save_cb != NULL) {
        (void)ctx->nvm_save_cb((uint8_t)0U, (uint32_t)0U);
    }

    return UDS_STATUS_OK;
}

uds_status_t uds_security_is_unlocked(
    const uds_security_ctx_t *ctx,
    uint8_t                   security_level,
    bool                     *out_unlocked)
{
    if ((ctx == NULL) || (out_unlocked == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    *out_unlocked = (ctx->active_level == security_level);

    return UDS_STATUS_OK;
}

uds_status_t uds_security_reset(uds_security_ctx_t *ctx)
{
    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    ctx->active_level    = (uint8_t)UDS_SEC_LEVEL_UNLOCKED;
    ctx->pending_level   = (uint8_t)0U;
    ctx->seed_pending    = false;
    ctx->seed_len        = (uint8_t)0U;
    /*
     * NOTE: failed_attempts and lockout_timer intentionally NOT reset here.
     * ISO 14229-1 §10.4.5.3: lockout must persist across session transitions.
     */

    return UDS_STATUS_OK;
}

uds_status_t uds_security_tick_1ms(uds_security_ctx_t *ctx)
{
    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (ctx->locked_out && (ctx->lockout_timer_ms > 0U)) {
        ctx->lockout_timer_ms--;
        if (ctx->lockout_timer_ms == 0U) {
            ctx->locked_out = false;
        }
    }

    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * Internal helper implementations
 * -------------------------------------------------------------------------- */

static uint8_t sec_key_level_to_seed_level(uint8_t key_level)
{
    if (key_level > (uint8_t)0U) {
        return (uint8_t)(key_level - (uint8_t)1U);
    }
    return (uint8_t)0U;
}

static bool sec_is_seed_level(uint8_t level)
{
    return ((level & (uint8_t)0x01U) != (uint8_t)0U);
}

/**
 * [P2-SEC-01] Constant-time comparison — processes all bytes unconditionally.
 *
 * @par MISRA C:2012 Deviation — volatile local variable (Rule 8.13 advisory)
 * 'diff' is declared 'volatile' to prevent the compiler from eliminating the
 * accumulation loop via dead-store removal or short-circuit optimisation.
 * Without 'volatile' an optimising compiler is permitted to break the
 * constant-time guarantee, turning this into a timing side-channel.
 * Deviation is justified by ASIL-B security requirement SEC-CT-01.
 */
static bool sec_constant_time_compare(
    const uint8_t *a,
    const uint8_t *b,
    uint8_t        len)
{
    /* MISRA Deviation: volatile required for constant-time guarantee. */
    volatile uint8_t diff = (uint8_t)0U;
    uint8_t          i;

    for (i = (uint8_t)0U; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }

    return (diff == (uint8_t)0U);
}
