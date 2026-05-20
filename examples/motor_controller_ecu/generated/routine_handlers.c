// File: generated/routine_handlers.c
// GENERATED — do NOT edit manually.
// ECU: MotorController_Inverter  v1.0.0  Generated: 2026-05-20T07:21:48Z

#include "routine_handlers.h"
#include "routine_database.h"
#include "uds_types.h"

/* =============================================================
 * Routine callback stubs
 * Replace each stub body with your ECU application logic.
 * ============================================================= */

/* RID 0xCC00 — MC_SelfTest : startRoutine stub */
uds_status_t routine_start_mc_selftest(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement routine logic */
    return UDS_STATUS_OK;
}

/* RID 0xCC00 — MC_SelfTest : requestRoutineResults stub */
uds_status_t routine_results_mc_selftest(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)result_buf; (void)result_buf_len;
    *result_len = 0U;
    /* TODO: return routine results */
    return UDS_STATUS_OK;
}

/* RID 0xCC01 — MC_PhaseBalanceTest : startRoutine stub */
uds_status_t routine_start_mc_phasebalancetest(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement routine logic */
    return UDS_STATUS_OK;
}

/* RID 0xCC01 — MC_PhaseBalanceTest : requestRoutineResults stub */
uds_status_t routine_results_mc_phasebalancetest(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)result_buf; (void)result_buf_len;
    *result_len = 0U;
    /* TODO: return routine results */
    return UDS_STATUS_OK;
}

/* RID 0xCC02 — MC_GateDriverFunctionalTest : startRoutine stub */
uds_status_t routine_start_mc_gatedriverfunctionaltest(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement routine logic */
    return UDS_STATUS_OK;
}

/* RID 0xCC02 — MC_GateDriverFunctionalTest : requestRoutineResults stub */
uds_status_t routine_results_mc_gatedriverfunctionaltest(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)result_buf; (void)result_buf_len;
    *result_len = 0U;
    /* TODO: return routine results */
    return UDS_STATUS_OK;
}

/* RID 0xCC10 — MC_ResolverOffsetCalibration : startRoutine stub */
uds_status_t routine_start_mc_resolveroffsetcalibration(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement routine logic */
    return UDS_STATUS_OK;
}

/* RID 0xCC10 — MC_ResolverOffsetCalibration : requestRoutineResults stub */
uds_status_t routine_results_mc_resolveroffsetcalibration(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)result_buf; (void)result_buf_len;
    *result_len = 0U;
    /* TODO: return routine results */
    return UDS_STATUS_OK;
}

/* RID 0xCC11 — MC_ForceMotorInhibit : startRoutine stub */
uds_status_t routine_start_mc_forcemotorinhibit(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement routine logic */
    return UDS_STATUS_OK;
}

/* RID 0xCC11 — MC_ForceMotorInhibit : stopRoutine stub */
uds_status_t routine_stop_mc_forcemotorinhibit(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement stop logic */
    return UDS_STATUS_OK;
}

/* RID 0xCC12 — MC_ClearFaultHistory : startRoutine stub */
uds_status_t routine_start_mc_clearfaulthistory(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement routine logic */
    return UDS_STATUS_OK;
}

/* Register all routines with routine_database */
uds_status_t routine_handlers_register_all(void)
{
    uds_status_t status;
    routine_entry_t entry;

    /* RID 0xCC00 — MC_SelfTest */
    entry.rid            = (uint16_t)52224U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_RESULTS);
    entry.min_session    = (uint8_t)UDS_SESSION_EXTENDED;
    entry.security_level = (uint8_t)0U;
    entry.start_cb       = routine_start_mc_selftest;
    entry.stop_cb        = NULL;
    entry.results_cb     = routine_results_mc_selftest;
    entry.description    = "MC_SelfTest";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    /* RID 0xCC01 — MC_PhaseBalanceTest */
    entry.rid            = (uint16_t)52225U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_RESULTS);
    entry.min_session    = (uint8_t)UDS_SESSION_EXTENDED;
    entry.security_level = (uint8_t)1U;
    entry.start_cb       = routine_start_mc_phasebalancetest;
    entry.stop_cb        = NULL;
    entry.results_cb     = routine_results_mc_phasebalancetest;
    entry.description    = "MC_PhaseBalanceTest";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    /* RID 0xCC02 — MC_GateDriverFunctionalTest */
    entry.rid            = (uint16_t)52226U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_RESULTS);
    entry.min_session    = (uint8_t)UDS_SESSION_EXTENDED;
    entry.security_level = (uint8_t)1U;
    entry.start_cb       = routine_start_mc_gatedriverfunctionaltest;
    entry.stop_cb        = NULL;
    entry.results_cb     = routine_results_mc_gatedriverfunctionaltest;
    entry.description    = "MC_GateDriverFunctionalTest";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    /* RID 0xCC10 — MC_ResolverOffsetCalibration */
    entry.rid            = (uint16_t)52240U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_RESULTS);
    entry.min_session    = (uint8_t)UDS_SESSION_PROGRAMMING;
    entry.security_level = (uint8_t)1U;
    entry.start_cb       = routine_start_mc_resolveroffsetcalibration;
    entry.stop_cb        = NULL;
    entry.results_cb     = routine_results_mc_resolveroffsetcalibration;
    entry.description    = "MC_ResolverOffsetCalibration";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    /* RID 0xCC11 — MC_ForceMotorInhibit */
    entry.rid            = (uint16_t)52241U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_STOP);
    entry.min_session    = (uint8_t)UDS_SESSION_EXTENDED;
    entry.security_level = (uint8_t)1U;
    entry.start_cb       = routine_start_mc_forcemotorinhibit;
    entry.stop_cb        = routine_stop_mc_forcemotorinhibit;
    entry.results_cb     = NULL;
    entry.description    = "MC_ForceMotorInhibit";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    /* RID 0xCC12 — MC_ClearFaultHistory */
    entry.rid            = (uint16_t)52242U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START);
    entry.min_session    = (uint8_t)UDS_SESSION_PROGRAMMING;
    entry.security_level = (uint8_t)1U;
    entry.start_cb       = routine_start_mc_clearfaulthistory;
    entry.stop_cb        = NULL;
    entry.results_cb     = NULL;
    entry.description    = "MC_ClearFaultHistory";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    return UDS_STATUS_OK;
}
