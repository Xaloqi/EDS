// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/eds_key_source.h
 *
 * PURPOSE: Install the SecurityAccess level keys from a key source (OTP fuses,
 *          or the application's own function) with the refusals #56 asks for,
 *          kept free of any RTOS so a host test can run it with a mocked
 *          source. platform/zephyr/zephyr_key_source.c is the Zephyr binding.
 *
 *          For each level (0x01, 0x03) the source is asked for a 16-byte key.
 *          A key that cannot be read, or reads all 0x00 (unprogrammed fuses)
 *          or all 0xFF (erased), is REFUSED. If any level is refused, a
 *          derivation callback that always fails is registered, so no key
 *          can ever match: SecurityAccess stays locked on every level and the
 *          compile-time placeholder keys cannot be used either. A seed is
 *          still issued; the tester's key is refused and counted by the
 *          existing attempt counter and lockout.
 *
 *          The key is held only in the caller's eds_key_scratch_t, which is
 *          wiped after every level, whether it was installed or refused.
 *
 * SAFETY  : Security-relevant. Not safety-assessed.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef EDS_KEY_SOURCE_H
#define EDS_KEY_SOURCE_H

#include "uds_security_algo.h"
#include "uds_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDS_KEY_LEN     16U   /* AES-128 */
#define EDS_KEY_LEVELS  2U    /* levels 0x01 and 0x03, as in uds_security_algo.c */

/** The levels whose keys come from the source, in the order of the result array. */
static const uint8_t EDS_KEY_LEVEL_IDS[EDS_KEY_LEVELS] = {0x01U, 0x03U};

/** The caller's stack buffer for one key at a time; wiped by eds_keys_install(). */
typedef struct {
    uint8_t key[EDS_KEY_LEN];
} eds_key_scratch_t;

typedef enum {
    EDS_KEY_INSTALLED   = 0,  /**< Key read and set for the level. */
    EDS_KEY_READ_FAILED = 1,  /**< The source reported an error. */
    EDS_KEY_ALL_ZERO    = 2,  /**< 16 x 0x00: unprogrammed fuses. Refused. */
    EDS_KEY_ALL_FF      = 3   /**< 16 x 0xFF: erased. Refused. */
} eds_key_state_t;

/**
 * @brief A key source. Writes EDS_KEY_LEN bytes of the key for `level` into key.
 * @return 0 on success, anything else on failure.
 */
typedef int (*eds_key_read_fn_t)(uint8_t level, uint8_t *key, void *ctx);

/**
 * @brief The integrator's key function, called for each level when the build
 *        selects CONFIG_EDS_KEYS_APP. EDS declares it and never defines it: a
 *        build that selects the option without defining it fails to link.
 *
 * Writes the EDS_KEY_LEN-byte key of `level` (0x01, 0x03) into key: from an HSM,
 * a hardware key store, a provisioning record, whatever the product has.
 *
 * @return 0 on success, anything else if the key is not available.
 */
int eds_app_get_level_key(uint8_t level, uint8_t *key);

/** Wipe that the compiler may not remove. */
static inline void eds_key_wipe(volatile uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0U; i < n; i++) {
        p[i] = 0U;
    }
}

/** Always fails: no key can match, so no SecurityAccess level unlocks. */
static inline uds_status_t eds_keys_deny_all_derive(uint8_t security_level, const uint8_t *seed,
                                                    uint8_t *key_out)
{
    (void)security_level;
    (void)seed;
    (void)key_out;
    return UDS_STATUS_ERR_SEC_ACCESS_DENIED;
}

/**
 * @brief Read and install the key of every level from `read`.
 *
 * @param read     The key source.
 * @param ctx      Passed to `read`.
 * @param scratch  The caller's buffer; zeroed before this returns.
 * @param states   EDS_KEY_LEVELS entries, set to each level's outcome.
 * @return true if every level was installed. false if any was refused: the
 *         deny-all derivation is then registered and SecurityAccess is locked.
 */
static inline bool eds_keys_install(eds_key_read_fn_t read, void *ctx, eds_key_scratch_t *scratch,
                                    eds_key_state_t states[EDS_KEY_LEVELS])
{
    bool all_ok = true;
    size_t lvl;

    if ((read == NULL) || (scratch == NULL) || (states == NULL)) {
        return false;
    }

    for (lvl = 0U; lvl < EDS_KEY_LEVELS; lvl++) {
        size_t i;
        uint8_t or_all = 0U;
        uint8_t and_all = 0xFFU;

        eds_key_wipe(scratch->key, sizeof(scratch->key));
        if (read(EDS_KEY_LEVEL_IDS[lvl], scratch->key, ctx) != 0) {
            states[lvl] = EDS_KEY_READ_FAILED;
        } else {
            for (i = 0U; i < EDS_KEY_LEN; i++) {
                or_all |= scratch->key[i];
                and_all &= scratch->key[i];
            }
            if (or_all == 0U) {
                states[lvl] = EDS_KEY_ALL_ZERO;
            } else if (and_all == 0xFFU) {
                states[lvl] = EDS_KEY_ALL_FF;
            } else if (uds_security_algo_set_level_key(EDS_KEY_LEVEL_IDS[lvl], scratch->key)
                       == UDS_STATUS_OK) {
                states[lvl] = EDS_KEY_INSTALLED;
            } else {
                states[lvl] = EDS_KEY_READ_FAILED;
            }
        }
        eds_key_wipe(scratch->key, sizeof(scratch->key));
        if (states[lvl] != EDS_KEY_INSTALLED) {
            all_ok = false;
        }
    }

    if (!all_ok) {
        uds_security_algo_set_derive_cb(eds_keys_deny_all_derive);
    }
    return all_ok;
}

#endif /* EDS_KEY_SOURCE_H */
