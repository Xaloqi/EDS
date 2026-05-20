// File: generated/routine_handlers.h
// GENERATED — do NOT edit manually.
// ECU: ARDEP_IOController  v1.0.0  Generated: 2026-05-20T07:21:48Z

#ifndef ROUTINE_HANDLERS_H
#define ROUTINE_HANDLERS_H

#include "uds_types.h"
#include "routine_database.h"

/* Register all routines from diagnostics_config.yaml */
uds_status_t routine_handlers_register_all(void);

/* RID 0xFF00 — ECU_SelfTest */
uds_status_t routine_start_ecu_selftest(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_ecu_selftest(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xFF01 — ResetCalibrationToDefaults */
uds_status_t routine_start_resetcalibrationtodefaults(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xFF10 — EraseApplicationFlash */
uds_status_t routine_start_eraseapplicationflash(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_eraseapplicationflash(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xFF11 — VerifyApplicationImage */
uds_status_t routine_start_verifyapplicationimage(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_verifyapplicationimage(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xFF20 — ActivateOutputChannel */
uds_status_t routine_start_activateoutputchannel(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_stop_activateoutputchannel(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_activateoutputchannel(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xFF21 — MeasureSupplyVoltage */
uds_status_t routine_start_measuresupplyvoltage(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_measuresupplyvoltage(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

#endif /* ROUTINE_HANDLERS_H */
