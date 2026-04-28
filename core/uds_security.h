// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_security.h
 *
 * PURPOSE: Public API for UDS security access management.
 *          Implements the seed/key exchange state machine per ISO 14229-1
 *          service 0x27 and governs access level gating.
 *
 * SAFETY  : Safety-critical interface. Changes require safety review.
 *           ASIL-B candidate per ISO 26262-6.
 *           Incorrect security implementation may permit unauthorized ECU access.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef UDS_SECURITY_H
#define UDS_SECURITY_H

#include "uds_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Configuration constants
 * -------------------------------------------------------------------------- */

/** Maximum consecutive failed security access attempts before lockout. */
#ifndef UDS_SECURITY_MAX_ATTEMPTS
#define UDS_SECURITY_MAX_ATTEMPTS (3U)
#endif

/** Security access lockout duration in milliseconds after exceeding attempt limit. */
#ifndef UDS_SECURITY_LOCKOUT_MS
#define UDS_SECURITY_LOCKOUT_MS (10000U)
#endif

/* --------------------------------------------------------------------------
 * Security level definitions
 *
 * ISO 14229-1 uses odd values for RequestSeed, even values for SendKey.
 * Level 0x01/0x02 is the lowest programmatic access level.
 * -------------------------------------------------------------------------- */
#define UDS_SEC_LEVEL_UNLOCKED   (0x00U) /**< No security active (default). */
#define UDS_SEC_LEVEL_1_SEED     (0x01U) /**< Level 1 seed request sub-function. */
#define UDS_SEC_LEVEL_1_KEY      (0x02U) /**< Level 1 key send sub-function. */
#define UDS_SEC_LEVEL_2_SEED     (0x03U) /**< Level 2 seed request sub-function. */
#define UDS_SEC_LEVEL_2_KEY      (0x04U) /**< Level 2 key send sub-function. */

/* --------------------------------------------------------------------------
 * Key validation callback prototype
 *
 * SAFETY: The actual key derivation algorithm must be application-provided.
 *         This stack does NOT implement a specific algorithm — it only
 *         provides the state machine framework.
 * -------------------------------------------------------------------------- */

/**
 * @brief Application-provided callback to validate a received security key.
 *
 * The callback receives the previously generated seed and the key submitted
 * by the tester and must return whether the key is valid.
 *
 * @param[in] security_level  Security level sub-function identifier.
 * @param[in] seed            Pointer to the seed bytes previously sent.
 * @param[in] seed_len        Length of the seed in bytes.
 * @param[in] key             Pointer to the key bytes received from tester.
 * @param[in] key_len         Length of the key in bytes.
 *
 * @return true if key is valid for the given seed and level.
 * @return false if key is invalid.
 *
 * @note SAFETY: This callback is security-critical. The implementing function
 *               must be evaluated at the required ASIL level.
 */
typedef bool (*uds_security_key_validate_fn)(
    uint8_t        security_level,
    const uint8_t *seed,
    uint8_t        seed_len,
    const uint8_t *key,
    uint8_t        key_len
);

/**
 * @brief Application-provided callback to generate a security seed.
 *
 * @param[in]  security_level  Security level sub-function identifier.
 * @param[out] seed_buf        Buffer to receive the generated seed bytes.
 * @param[in]  seed_buf_len    Size of seed_buf in bytes.
 * @param[out] out_seed_len    Number of seed bytes written.
 *
 * @return UDS_STATUS_OK if seed was generated successfully.
 * @return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE if seed generation failed.
 *
 * @note SAFETY: Seed generation must use a hardware RNG source where available.
 *               Predictable seeds undermine the security model.
 */
typedef uds_status_t (*uds_security_seed_generate_fn)(
    uint8_t  security_level,
    uint8_t *seed_buf,
    uint8_t  seed_buf_len,
    uint8_t *out_seed_len
);

/* --------------------------------------------------------------------------
 * Security context
 * -------------------------------------------------------------------------- */

/**
 * @brief UDS security access management context.
 *
 * SAFETY: Caller must not modify fields directly.
 */
typedef struct uds_security_ctx {
    bool                          initialized;                       /**< Initialization guard. */
    uint8_t                       active_level;                      /**< Currently unlocked security level. */
    uint8_t                       pending_level;                     /**< Level awaiting key response. */
    uint8_t                       seed[UDS_SECURITY_SEED_LEN];       /**< Last generated seed. */
    uint8_t                       seed_len;                          /**< Valid bytes in seed[]. */
    bool                          seed_pending;                      /**< True if seed has been sent, key awaited. */
    uint8_t                       failed_attempts;                   /**< Consecutive failed key attempts. */
    uint8_t                       max_attempts;                      /**< Max failed attempts before lockout. */
    uint32_t                      lockout_timer_ms;                  /**< Lockout countdown timer (ms). */
    uint32_t                      lockout_duration_ms;               /**< Configured lockout duration (ms). */
    bool                          locked_out;                        /**< True if in lockout period. */
    uds_security_key_validate_fn  key_validate_cb;                   /**< Application key validation callback. */
    uds_security_seed_generate_fn seed_generate_cb;                  /**< Application seed generation callback. */
    /* [P3-SEC-01] NVM persistence callbacks (optional — NULL if not wired). */
    uds_status_t (*nvm_load_cb)(uint8_t *out_attempts, uint32_t *out_lockout_ms);
    uds_status_t (*nvm_save_cb)(uint8_t attempts, uint32_t lockout_ms);
} uds_security_ctx_t;

/* --------------------------------------------------------------------------
 * Security configuration
 * -------------------------------------------------------------------------- */

/**
 * @brief Static configuration for the security module.
 */
typedef struct uds_security_cfg {
    uint8_t                       max_attempts;        /**< Maximum failed attempts before lockout. */
    uint32_t                      lockout_ms;          /**< Lockout duration in milliseconds. */
    uds_security_key_validate_fn  key_validate_cb;     /**< Required: application key validation callback. */
    uds_security_seed_generate_fn seed_generate_cb;    /**< Required: application seed generation callback. */

    /**
     * @brief [P3-SEC-01] Load persisted attempt counter from NVM. Optional.
     *
     * Called during uds_security_init() to restore failed-attempt count
     * and lockout timer residual from NVM, preventing counter reset by
     * power-cycling the ECU.
     *
     * Prototype: uds_status_t fn(uint8_t *out_attempts, uint32_t *out_lockout_ms)
     * Return UDS_STATUS_ERR_DID_NOT_FOUND on first boot (no persisted data).
     * If NULL, counter starts at zero each power-on.
     */
    uds_status_t (*nvm_load_cb)(uint8_t *out_attempts, uint32_t *out_lockout_ms);

    /**
     * @brief [P3-SEC-01] Save attempt counter to NVM. Optional.
     *
     * Called after each failed attempt and on lockout engage.
     * If NULL, counter is not persisted.
     *
     * Prototype: uds_status_t fn(uint8_t attempts, uint32_t lockout_ms)
     * Failure is non-fatal — logged but does not abort the request.
     */
    uds_status_t (*nvm_save_cb)(uint8_t attempts, uint32_t lockout_ms);

} uds_security_cfg_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialize the security access module.
 *
 * @param[out] ctx  Caller-allocated security context.
 * @param[in]  cfg  Security configuration block.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if callbacks are NULL.
 * @return UDS_STATUS_ERR_ALREADY_INITIALIZED if already initialized.
 */
uds_status_t uds_security_init(
    uds_security_ctx_t       *ctx,
    const uds_security_cfg_t *cfg
);

/**
 * @brief Process a seed request (odd sub-function value).
 *
 * Generates a seed and stores it internally pending key reception.
 * If the requested level is already unlocked, returns an all-zero seed
 * per ISO 14229-1 specification.
 *
 * @param[in]  ctx            Initialized security context.
 * @param[in]  security_level Seed sub-function value (odd: 0x01, 0x03, ...).
 * @param[out] seed_buf       Buffer to receive generated seed bytes.
 * @param[in]  seed_buf_len   Size of seed_buf.
 * @param[out] out_seed_len   Number of seed bytes written.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if not initialized.
 * @return UDS_STATUS_ERR_SEC_ATTEMPT_EXCEEDED if in lockout.
 * @return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE if seed generation failed.
 *
 * @note SAFETY: Safety-critical path. ASIL-B relevant.
 */
uds_status_t uds_security_request_seed(
    uds_security_ctx_t *ctx,
    uint8_t             security_level,
    uint8_t            *seed_buf,
    uint8_t             seed_buf_len,
    uint8_t            *out_seed_len
);

/**
 * @brief Process a key submission (even sub-function value).
 *
 * Validates the submitted key against the pending seed. On success,
 * unlocks the corresponding security level. On failure, increments
 * the attempt counter and may engage lockout.
 *
 * @param[in] ctx            Initialized security context.
 * @param[in] security_level Key sub-function value (even: 0x02, 0x04, ...).
 * @param[in] key            Pointer to submitted key bytes.
 * @param[in] key_len        Length of submitted key in bytes.
 *
 * @return UDS_STATUS_OK if key accepted and level is unlocked.
 * @return UDS_STATUS_ERR_SEC_INVALID_KEY if key does not match.
 * @return UDS_STATUS_ERR_SEC_ATTEMPT_EXCEEDED if lockout engaged.
 * @return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE if no seed is pending.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 *
 * @note SAFETY: Safety-critical path. Must not expose timing side-channels.
 */
uds_status_t uds_security_send_key(
    uds_security_ctx_t *ctx,
    uint8_t             security_level,
    const uint8_t      *key,
    uint8_t             key_len
);

/**
 * @brief Query whether a given security level is currently unlocked.
 *
 * @param[in]  ctx            Initialized security context.
 * @param[in]  security_level Level to query (use UDS_SEC_LEVEL_* defines).
 * @param[out] out_unlocked   Set to true if the level is unlocked.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if not initialized.
 */
uds_status_t uds_security_is_unlocked(
    const uds_security_ctx_t *ctx,
    uint8_t                   security_level,
    bool                     *out_unlocked
);

/**
 * @brief Reset security access to locked state.
 *
 * Clears all unlocked levels and resets attempt counters.
 * Must be called on session transition to DEFAULT.
 *
 * @param[in] ctx  Initialized security context.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if ctx is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if not initialized.
 */
uds_status_t uds_security_reset(uds_security_ctx_t *ctx);

/**
 * @brief 1 ms periodic tick for lockout timer management.
 *
 * @param[in] ctx  Initialized security context.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if ctx is NULL.
 *
 * @note TIMING: Must be called at 1 ms resolution.
 */
uds_status_t uds_security_tick_1ms(uds_security_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* UDS_SECURITY_H */
