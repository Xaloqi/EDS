// File: generated/routine_handlers.h
// GENERATED — do NOT edit manually.
// ECU: BasicECU_DoIP  v1.6.0  Generated: 2026-05-20T07:21:47Z

#ifndef ROUTINE_HANDLERS_H
#define ROUTINE_HANDLERS_H

#include "uds_types.h"
#include "routine_database.h"

/* Register all routines from diagnostics_config.yaml */
uds_status_t routine_handlers_register_all(void);

/* RID 0xFF00 — ECU_SelfTest */
uds_status_t routine_start_ecu_selftest(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_ecu_selftest(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xFF01 — ResetToFactoryDefaults */
uds_status_t routine_start_resettofactorydefaults(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xFF10 — EraseNVM */
uds_status_t routine_start_erasenvm(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_erasenvm(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

#endif /* ROUTINE_HANDLERS_H */
