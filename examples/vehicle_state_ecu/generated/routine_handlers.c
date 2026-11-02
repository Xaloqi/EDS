// File: generated/routine_handlers.c
// GENERATED — do NOT edit manually, with ONE documented exception below.
// ECU: VehicleStateECU  v0.1.0  Generated: 2026-10-04T10:24:58Z
//
// [APP HOOK] This file normally regenerates as pure TODO stubs (see every
// other example's generated/routine_handlers.c). This one example departs
// from that, intentionally: the four bodies below delegate to
// src/vehicle_state.c instead of returning an empty stub result, so the
// RoutineControl actuation path (RID 0xC100 / 0xC101) does something real.
//
// There is no other way to give a routine live behaviour in this codebase —
// routine_database_find() returns a `const routine_entry_t *`, so
// src/main.c cannot re-point a callback at runtime; the callback bodies
// here are the only place application logic can run (see EDS#340 for the
// cross-example survey that confirmed this). Each [APP HOOK] marker below
// is exactly what to re-apply if this file is ever regenerated from the
// YAML (examples/vehicle_state_ecu/CMakeLists.txt sets DIAG_SKIP_CODEGEN=ON
// by default for exactly this reason, and a configure-time check fails
// loudly if these markers go missing).

#include "routine_handlers.h"
#include "routine_database.h"
#include "uds_types.h"
#include "vehicle_state.h"

/* [APP HOOK] RID 0xC100 — IndicatorOutputSelfTest : startRoutine */
uds_status_t routine_start_indicatoroutputselftest(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len;
    return vehicle_state_selftest_start(result_buf, result_buf_len, result_len);
}

/* [APP HOOK] RID 0xC100 — IndicatorOutputSelfTest : requestRoutineResults */
uds_status_t routine_results_indicatoroutputselftest(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    return vehicle_state_selftest_results(result_buf, result_buf_len, result_len);
}

/* [APP HOOK] RID 0xC101 — IndicatorLampCheck : startRoutine */
uds_status_t routine_start_indicatorlampcheck(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len;
    return vehicle_state_lampcheck_start(result_buf, result_buf_len, result_len);
}

/* [APP HOOK] RID 0xC101 — IndicatorLampCheck : stopRoutine */
uds_status_t routine_stop_indicatorlampcheck(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len;
    return vehicle_state_lampcheck_stop(result_buf, result_buf_len, result_len);
}

/* Register all routines with routine_database */
uds_status_t routine_handlers_register_all(void)
{
    uds_status_t status;
    routine_entry_t entry;

    /* RID 0xC100 — IndicatorOutputSelfTest */
    entry.rid            = (uint16_t)49408U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_RESULTS);
    entry.min_session    = (uint8_t)UDS_SESSION_EXTENDED;
    entry.security_level = (uint8_t)0U;
    entry.start_cb       = routine_start_indicatoroutputselftest;
    entry.stop_cb        = NULL;
    entry.results_cb     = routine_results_indicatoroutputselftest;
    entry.description    = "IndicatorOutputSelfTest";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    /* RID 0xC101 — IndicatorLampCheck */
    entry.rid            = (uint16_t)49409U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_STOP);
    entry.min_session    = (uint8_t)UDS_SESSION_EXTENDED;
    entry.security_level = (uint8_t)1U;
    entry.start_cb       = routine_start_indicatorlampcheck;
    entry.stop_cb        = routine_stop_indicatorlampcheck;
    entry.results_cb     = NULL;
    entry.description    = "IndicatorLampCheck";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    return UDS_STATUS_OK;
}
