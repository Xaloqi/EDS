// File: generated/routine_handlers.h
// GENERATED — do NOT edit manually.
// ECU: RobotJointController  v1.0.0  Generated: 2026-05-19T13:34:41Z

#ifndef ROUTINE_HANDLERS_H
#define ROUTINE_HANDLERS_H

#include "uds_types.h"
#include "routine_database.h"

/* Register all routines from diagnostics_config.yaml */
uds_status_t routine_handlers_register_all(void);

/* RID 0xFF00 — HomeAxis0 */
uds_status_t routine_start_homeaxis0(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_homeaxis0(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xFF01 — ApplyCalibration */
uds_status_t routine_start_applycalibration(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_applycalibration(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xFF02 — ClearFaultHistory */
uds_status_t routine_start_clearfaulthistory(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

#endif /* ROUTINE_HANDLERS_H */
