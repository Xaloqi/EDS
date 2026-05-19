// File: generated/routine_handlers.c
// GENERATED — do NOT edit manually.
// ECU: BMS_MainController  v1.0.0  Generated: 2026-05-19T13:34:40Z

#include "routine_handlers.h"
#include "routine_database.h"
#include "uds_types.h"

/* =============================================================
 * Routine callback stubs
 * Replace each stub body with your ECU application logic.
 * ============================================================= */

/* RID 0xBB00 — BMS_SelfTest : startRoutine stub */
uds_status_t routine_start_bms_selftest(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement routine logic */
    return UDS_STATUS_OK;
}

/* RID 0xBB00 — BMS_SelfTest : requestRoutineResults stub */
uds_status_t routine_results_bms_selftest(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)result_buf; (void)result_buf_len;
    *result_len = 0U;
    /* TODO: return routine results */
    return UDS_STATUS_OK;
}

/* RID 0xBB01 — BMS_ForcePassiveBalance : startRoutine stub */
uds_status_t routine_start_bms_forcepassivebalance(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement routine logic */
    return UDS_STATUS_OK;
}

/* RID 0xBB01 — BMS_ForcePassiveBalance : stopRoutine stub */
uds_status_t routine_stop_bms_forcepassivebalance(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement stop logic */
    return UDS_STATUS_OK;
}

/* RID 0xBB01 — BMS_ForcePassiveBalance : requestRoutineResults stub */
uds_status_t routine_results_bms_forcepassivebalance(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)result_buf; (void)result_buf_len;
    *result_len = 0U;
    /* TODO: return routine results */
    return UDS_STATUS_OK;
}

/* RID 0xBB02 — BMS_ContactorFunctionalTest : startRoutine stub */
uds_status_t routine_start_bms_contactorfunctionaltest(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement routine logic */
    return UDS_STATUS_OK;
}

/* RID 0xBB02 — BMS_ContactorFunctionalTest : requestRoutineResults stub */
uds_status_t routine_results_bms_contactorfunctionaltest(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)result_buf; (void)result_buf_len;
    *result_len = 0U;
    /* TODO: return routine results */
    return UDS_STATUS_OK;
}

/* RID 0xBB10 — BMS_ResetSoCToFullCharge : startRoutine stub */
uds_status_t routine_start_bms_resetsoctofullcharge(
    const uint8_t *opt_buf, uint8_t opt_len,
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    (void)opt_buf; (void)opt_len; (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;
    /* TODO: implement routine logic */
    return UDS_STATUS_OK;
}

/* RID 0xBB11 — BMS_ClearFaultHistory : startRoutine stub */
uds_status_t routine_start_bms_clearfaulthistory(
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

    /* RID 0xBB00 — BMS_SelfTest */
    entry.rid            = (uint16_t)47872U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_RESULTS);
    entry.min_session    = (uint8_t)UDS_SESSION_EXTENDED;
    entry.security_level = (uint8_t)0U;
    entry.start_cb       = routine_start_bms_selftest;
    entry.stop_cb        = NULL;
    entry.results_cb     = routine_results_bms_selftest;
    entry.description    = "BMS_SelfTest";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    /* RID 0xBB01 — BMS_ForcePassiveBalance */
    entry.rid            = (uint16_t)47873U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_STOP | ROUTINE_SUPPORT_RESULTS);
    entry.min_session    = (uint8_t)UDS_SESSION_EXTENDED;
    entry.security_level = (uint8_t)1U;
    entry.start_cb       = routine_start_bms_forcepassivebalance;
    entry.stop_cb        = routine_stop_bms_forcepassivebalance;
    entry.results_cb     = routine_results_bms_forcepassivebalance;
    entry.description    = "BMS_ForcePassiveBalance";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    /* RID 0xBB02 — BMS_ContactorFunctionalTest */
    entry.rid            = (uint16_t)47874U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START | ROUTINE_SUPPORT_RESULTS);
    entry.min_session    = (uint8_t)UDS_SESSION_EXTENDED;
    entry.security_level = (uint8_t)1U;
    entry.start_cb       = routine_start_bms_contactorfunctionaltest;
    entry.stop_cb        = NULL;
    entry.results_cb     = routine_results_bms_contactorfunctionaltest;
    entry.description    = "BMS_ContactorFunctionalTest";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    /* RID 0xBB10 — BMS_ResetSoCToFullCharge */
    entry.rid            = (uint16_t)47888U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START);
    entry.min_session    = (uint8_t)UDS_SESSION_PROGRAMMING;
    entry.security_level = (uint8_t)1U;
    entry.start_cb       = routine_start_bms_resetsoctofullcharge;
    entry.stop_cb        = NULL;
    entry.results_cb     = NULL;
    entry.description    = "BMS_ResetSoCToFullCharge";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    /* RID 0xBB11 — BMS_ClearFaultHistory */
    entry.rid            = (uint16_t)47889U;
    entry.support_flags  = (uint8_t)(ROUTINE_SUPPORT_START);
    entry.min_session    = (uint8_t)UDS_SESSION_PROGRAMMING;
    entry.security_level = (uint8_t)1U;
    entry.start_cb       = routine_start_bms_clearfaulthistory;
    entry.stop_cb        = NULL;
    entry.results_cb     = NULL;
    entry.description    = "BMS_ClearFaultHistory";
    status = routine_database_register(&entry);
    if (status != UDS_STATUS_OK) { return status; }

    return UDS_STATUS_OK;
}
