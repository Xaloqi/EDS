// File: generated/routine_handlers.h
// GENERATED — do NOT edit manually.
// ECU: SafeBootFreeRTOSECU  v1.0.0  Generated: 2026-06-22T00:00:00Z

#ifndef ROUTINE_HANDLERS_H
#define ROUTINE_HANDLERS_H

#include "uds_types.h"
#include "routine_database.h"

/* Register all routines from diagnostics_config.yaml */
uds_status_t routine_handlers_register_all(void);

/* RID 0xFF00 — CheckProgrammingPreconditions */
uds_status_t routine_start_checkprogrammingpreconditions(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_checkprogrammingpreconditions(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xFF01 — VerifyOTASlotIntegrity */
uds_status_t routine_start_verifyotaslotintegrity(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_verifyotaslotintegrity(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

#endif /* ROUTINE_HANDLERS_H */
