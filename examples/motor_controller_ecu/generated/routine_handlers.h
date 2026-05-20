// File: generated/routine_handlers.h
// GENERATED — do NOT edit manually.
// ECU: MotorController_Inverter  v1.0.0  Generated: 2026-05-20T07:21:48Z

#ifndef ROUTINE_HANDLERS_H
#define ROUTINE_HANDLERS_H

#include "uds_types.h"
#include "routine_database.h"

/* Register all routines from diagnostics_config.yaml */
uds_status_t routine_handlers_register_all(void);

/* RID 0xCC00 — MC_SelfTest */
uds_status_t routine_start_mc_selftest(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_mc_selftest(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xCC01 — MC_PhaseBalanceTest */
uds_status_t routine_start_mc_phasebalancetest(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_mc_phasebalancetest(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xCC02 — MC_GateDriverFunctionalTest */
uds_status_t routine_start_mc_gatedriverfunctionaltest(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_mc_gatedriverfunctionaltest(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xCC10 — MC_ResolverOffsetCalibration */
uds_status_t routine_start_mc_resolveroffsetcalibration(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_mc_resolveroffsetcalibration(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xCC11 — MC_ForceMotorInhibit */
uds_status_t routine_start_mc_forcemotorinhibit(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_stop_mc_forcemotorinhibit(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xCC12 — MC_ClearFaultHistory */
uds_status_t routine_start_mc_clearfaulthistory(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

#endif /* ROUTINE_HANDLERS_H */
