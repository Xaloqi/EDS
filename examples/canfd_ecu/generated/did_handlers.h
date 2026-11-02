/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: GENERATED — did_handlers.h
 *
 * ECU       : CanFdEcu
 * Version   : 1.0.0
 * Generated : 2026-11-02T08:21:02Z
 *
 * PURPOSE: Declarations for all generated DID read/write handler functions
 *          and the generated DID registration entry point.
 *
 *          Architecture:
 *            diagnostics_config.yaml (dids section)
 *              -> tools/codegen.py (build_did_handlers_context)
 *                -> tools/templates/did_handlers.h.j2
 *                  -> generated/did_handlers.h  (this file)
 *
 *          This design allows DID handler stubs to be automatically generated
 *          from YAML configuration without modifying any core stack headers.
 *
 * WARNING: DO NOT EDIT MANUALLY.
 *          Regenerate: python3 tools/codegen.py --config <yaml> --out generated/
 *
 * SAFETY  : DID handlers provide read access to ECU measurement data and
 *           write access to configuration data. Session and security gating
 *           is enforced at the service layer (service_0x22.c / service_0x2E.c).
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef DID_HANDLERS_H
#define DID_HANDLERS_H

#include "uds_types.h"
#include "did_database.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Generated DID count for this ECU configuration.
 * Must match GEN_DID_COUNT in generated_config.h.
 */
#define GEN_DID_HANDLER_COUNT (5U)

/* --------------------------------------------------------------------------
 * Generated DID read handler declarations
 *
 * One function per DID that has 'read' in its access list.
 * All conform to the did_read_cb_fn prototype from did_database.h.
 * -------------------------------------------------------------------------- */

/**
 * @brief Read handler for DID 0xF190 — FdEcuIdentification.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 17 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for FdEcuIdentification.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 17).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_fdecuidentification(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x0100 — FdRecordA.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 64 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for FdRecordA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 64).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_fdrecorda(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x0101 — FdRecordB.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 64 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for FdRecordB.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 64).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_fdrecordb(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x0102 — FdRecordC.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 64 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for FdRecordC.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 64).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_fdrecordc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xF201 — FdPeriodicStatus.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 20 byte(s)
 *
 * IMPLEMENTED BY THE APPLICATION (source: app). Not generated: define this
 * function in application code. It is called only after the service layer
 * or did_safe_read_fdperiodicstatus() has checked session and security.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 20).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_fdperiodicstatus(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/* --------------------------------------------------------------------------
 * Generated DID write handler declarations
 * -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * Registration entry point
 * -------------------------------------------------------------------------- */

/**
 * @brief Register all generated DID entries with the DID database.
 *
 * Must be called after did_database_init() and before any UDS requests
 * are processed. Called by uds_generated_init().
 *
 * @return UDS_STATUS_OK if all DIDs registered successfully.
 * @return UDS_STATUS_ERR_* if any registration fails.
 */
uds_status_t did_handlers_register_all(void);

#ifdef __cplusplus
}
#endif

#endif /* DID_HANDLERS_H */
