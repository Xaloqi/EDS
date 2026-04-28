// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_session_stats.h
 *
 * PURPOSE: Session statistics — persist diagnostic lifecycle counters across
 *          ECU resets.
 *
 *          Tracks:
 *            - Total ECU resets
 *            - Programming session entry count
 *            - Extended session entry count
 *            - Security unlock count
 *            - Security lockout count
 *
 *          Wire point: register the session change callback with
 *          uds_session_register_change_cb() to auto-update counters.
 *
 * SAFETY  : Informational only — no ASIL requirement.
 * =============================================================================
 */

#ifndef UDS_SESSION_STATS_H
#define UDS_SESSION_STATS_H

#include "uds_types.h"
#include "nvm_store.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the session statistics module.
 *
 * Loads persisted stats from NVM (NVM_KEY_SESSION_STATS).
 * Must be called after nvm_store_init().
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_ALREADY_INITIALIZED if already initialized.
 */
uds_status_t uds_session_stats_init(void);

/**
 * @brief Session change callback — update statistics on transition.
 *
 * Pass this to uds_session_register_change_cb() to auto-update counters
 * whenever the session changes (including S3 timeout → DEFAULT).
 *
 * @param[in] old_session  Previous session type.
 * @param[in] new_session  New session type.
 */
void uds_session_stats_on_change(
    uds_session_type_t old_session,
    uds_session_type_t new_session);

/**
 * @brief Record a security event in the statistics.
 *
 * @param[in] was_unlock  true = successful unlock, false = lockout triggered.
 */
void uds_session_stats_record_security(bool was_unlock);

/**
 * @brief Flush session statistics to NVM.
 *
 * Called from zephyr_port_nvm_flush() before ECU reset.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_PLATFORM if NVM write failed.
 */
uds_status_t uds_session_stats_flush(void);

/**
 * @brief Read the current session statistics (live RAM copy).
 *
 * @param[out] out_stats  Populated with current counter values.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if out_stats is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if not initialized.
 */
uds_status_t uds_session_stats_get(nvm_session_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* UDS_SESSION_STATS_H */
