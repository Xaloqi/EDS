// File: generated/routine_handlers.c
// GENERATED — do NOT edit manually.
// ECU: VehicleStateECU  v0.1.0  Generated: 2026-11-15T10:18:13Z

#include "routine_handlers.h"
#include "routine_database.h"
#include "uds_types.h"

/* Defined by the application (source: app), not here:
 *   RID 0xC100 — IndicatorOutputSelfTest: routine_start_indicatoroutputselftest(), routine_results_indicatoroutputselftest()
 *   RID 0xC101 — IndicatorLampCheck: routine_start_indicatorlampcheck(), routine_stop_indicatorlampcheck()
 */

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
