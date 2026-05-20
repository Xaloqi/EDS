// File: generated/routine_handlers.h
// GENERATED — do NOT edit manually.
// ECU: SafeBootECU  v1.0.0  Generated: 2026-05-20T07:21:49Z

#ifndef ROUTINE_HANDLERS_H
#define ROUTINE_HANDLERS_H

#include "uds_types.h"
#include "routine_database.h"

/* Register all routines from diagnostics_config.yaml */
uds_status_t routine_handlers_register_all(void);

/* RID 0xFF00 — CheckProgrammingPreconditions */
uds_status_t routine_start_checkprogrammingpreconditions(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_checkprogrammingpreconditions(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/* RID 0xFF01 — VerifyBootloaderIntegrity */
uds_status_t routine_start_verifybootloaderintegrity(const uint8_t *opt_buf, uint8_t opt_len, uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);
uds_status_t routine_results_verifybootloaderintegrity(uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

#endif /* ROUTINE_HANDLERS_H */
