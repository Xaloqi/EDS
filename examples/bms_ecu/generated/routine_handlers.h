// File: generated/routine_handlers.h
// GENERATED — do NOT edit manually.
// ECU: BMS_MainController  v1.0.0  Generated: 2026-05-19T13:34:40Z

#ifndef ROUTINE_HANDLERS_H
#define ROUTINE_HANDLERS_H

#include "uds_types.h"
#include "routine_database.h"

/* Register all routines from diagnostics_config.yaml */
uds_status_t routine_handlers_register_all(void);

/* RID 0xBB00 — BMS_SelfTest */
uds_status_t routine_start_bms_selftest(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_bms_selftest(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xBB01 — BMS_ForcePassiveBalance */
uds_status_t routine_start_bms_forcepassivebalance(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_stop_bms_forcepassivebalance(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_bms_forcepassivebalance(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xBB02 — BMS_ContactorFunctionalTest */
uds_status_t routine_start_bms_contactorfunctionaltest(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_bms_contactorfunctionaltest(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xBB10 — BMS_ResetSoCToFullCharge */
uds_status_t routine_start_bms_resetsoctofullcharge(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xBB11 — BMS_ClearFaultHistory */
uds_status_t routine_start_bms_clearfaulthistory(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

#endif /* ROUTINE_HANDLERS_H */
