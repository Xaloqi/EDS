// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/vehicle_state_ecu/src/diag_app.c
 *
 * PURPOSE: The application's diagnostic handlers:
 *          RoutineControl for RIDs 0xC100/0xC101, delegating to
 *          vehicle_state.c.
 *          diagnostics_config.yaml marks these 'source: app', so codegen
 *          declares them in generated/routine_handlers.h and registers
 *          them behind the same session and security checks, but emits no
 *          body; the bodies live here, outside generated/, and survive
 *          regeneration.
 *          A missing one is a link error. Before EDS#613 these bodies were
 *          hand edits inside generated/, which regeneration overwrote.
 * =============================================================================
 */

#include "routine_handlers.h"
#include "uds_types.h"
#include "vehicle_state.h"

/* RID 0xC100 — IndicatorOutputSelfTest : startRoutine */
uds_status_t routine_start_indicatoroutputselftest(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len;
    return vehicle_state_selftest_start(result_buf, result_buf_len, result_len);
}

/* RID 0xC100 — IndicatorOutputSelfTest : requestRoutineResults */
uds_status_t routine_results_indicatoroutputselftest(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    return vehicle_state_selftest_results(result_buf, result_buf_len, result_len);
}

/* RID 0xC101 — IndicatorLampCheck : startRoutine */
uds_status_t routine_start_indicatorlampcheck(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len;
    return vehicle_state_lampcheck_start(result_buf, result_buf_len, result_len);
}

/* RID 0xC101 — IndicatorLampCheck : stopRoutine */
uds_status_t routine_stop_indicatorlampcheck(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len;
    return vehicle_state_lampcheck_stop(result_buf, result_buf_len, result_len);
}
