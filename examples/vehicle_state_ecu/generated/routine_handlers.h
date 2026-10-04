// File: generated/routine_handlers.h
// GENERATED — do NOT edit manually.
// ECU: VehicleStateECU  v0.1.0  Generated: 2026-10-04T10:24:58Z

#ifndef ROUTINE_HANDLERS_H
#define ROUTINE_HANDLERS_H

#include "uds_types.h"
#include "routine_database.h"

/* Register all routines from diagnostics_config.yaml */
uds_status_t routine_handlers_register_all(void);

/* RID 0xC100 — IndicatorOutputSelfTest */
uds_status_t routine_start_indicatoroutputselftest(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_indicatoroutputselftest(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xC101 — IndicatorLampCheck */
uds_status_t routine_start_indicatorlampcheck(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_stop_indicatorlampcheck(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

#endif /* ROUTINE_HANDLERS_H */
