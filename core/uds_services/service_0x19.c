// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x19.c
 *
 * PURPOSE: SID 0x19 — ReadDTCInformation service handler.
 *
 * ISO 14229-1 §13 — ReadDTCInformation
 *
 * This is the most complex UDS service — it provides the complete fault-reading
 * interface used by every OEM diagnostic tool and EOL tester.
 *
 * IMPLEMENTED SUB-FUNCTIONS:
 *
 *   0x01 — reportNumberOfDTCByStatusMask
 *     Request : [0x19, 0x01, statusMask]
 *     Response: [0x59, 0x01, availabilityMask, fmtID, countHB, countLB]
 *     Returns the count of DTCs whose (status_byte & statusMask) != 0.
 *
 *   0x02 — reportDTCByStatusMask
 *     Request : [0x19, 0x02, statusMask]
 *     Response: [0x59, 0x02, availabilityMask, {dtc3B, statusByte}...]
 *     Returns a list of all matching DTCs with their current status bytes.
 *
 *   0x03 — reportDTCSnapshotIdentification (not required per core profile —
 *           returns empty list; tester must accept this per ISO 14229-1 §13.3.3)
 *
 *   0x04 — reportDTCSnapshotRecordByDTCNumber
 *     Request : [0x19, 0x04, dtcHB, dtcMB, dtcLB, snapshotRecordNumber]
 *     Response: [0x59, 0x04, dtcHB, dtcMB, dtcLB, statusByte,
 *                snapshotRecordNumber, numberOfIdentifiers=0x00]
 *     Minimal snapshot: no freeze-frame data stored in this implementation.
 *     Returns the DTC + status + empty snapshot record (ISO-compliant).
 *
 *   0x06 — reportDTCExtDataRecordByDTCNumber
 *     Request : [0x19, 0x06, dtcHB, dtcMB, dtcLB, extDataRecordNumber]
 *     Response: [0x59, 0x06, dtcHB, dtcMB, dtcLB, statusByte,
 *                extDataRecordNumber, recordLength=0x00]
 *     Minimal extended data: no extended data stored in this implementation.
 *
 *   0x0A — reportSupportedDTCs
 *     Request : [0x19, 0x0A]
 *     Response: [0x59, 0x0A, availabilityMask, {dtc3B, statusByte}...]
 *     Returns all registered DTCs regardless of status.
 *
 * DTC FORMAT IN RESPONSES (ISO 14229-1 §D.2):
 *   [DTC_HB, DTC_MB, DTC_LB, statusByte] — 4 bytes per DTC
 *   DTC bytes are the 3 most-significant bytes of the 24-bit DTC code.
 *
 * STATUS AVAILABILITY MASK:
 *   Bit mask indicating which DTC status bits this ECU supports.
 *   This implementation supports all 8 bits (0xFF).
 *
 * DTC FORMAT IDENTIFIER:
 *   0x01 = ISO 14229-1 DTC format (used in reportNumberOfDTC response).
 *
 * SESSION REQUIREMENT (ISO 14229-1 §13.2.2):
 *   Available in all diagnostic sessions.
 *
 * SAFETY  : ASIL-B candidate. Returns safety-relevant fault data.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "dtc_database.h"

#include <string.h>

/* --------------------------------------------------------------------------
 * Sub-function identifiers (ISO 14229-1 Table 239)
 * -------------------------------------------------------------------------- */
#define SVC_0x19_SUBFN_REPORT_NUM_BY_STATUS_MASK     (0x01U)
#define SVC_0x19_SUBFN_REPORT_DTC_BY_STATUS_MASK     (0x02U)
#define SVC_0x19_SUBFN_REPORT_DTC_SNAPSHOT_ID        (0x03U)
#define SVC_0x19_SUBFN_REPORT_DTC_SNAPSHOT_BY_DTC    (0x04U)
#define SVC_0x19_SUBFN_REPORT_DTC_EXT_DATA_BY_DTC    (0x06U)
#define SVC_0x19_SUBFN_REPORT_SUPPORTED_DTCS         (0x0AU)

/** suppressPosRspMsgIndicationBit mask for sub-function byte. */
#define SVC_0x19_SUPPRESS_BIT                         (0x80U)

/** Mask for actual sub-function value (bits 6..0). */
#define SVC_0x19_SUBFN_MASK                           (0x7FU)

/**
 * @brief Status availability mask reported to the tester.
 *
 * 0xFF indicates that this ECU supports all 8 status bits defined in
 * ISO 14229-1 Table D.1. Restricting this mask to the bits actually
 * supported by the application prevents confusing tester interpretations.
 */
#define SVC_0x19_STATUS_AVAILABILITY_MASK             (0xFFU)

/**
 * @brief DTC format identifier (ISO 14229-1 Table 250).
 * 0x01 = ISO 14229-1 DTC format.
 */
#define SVC_0x19_DTC_FORMAT_ISO14229                  (0x01U)

/** Bytes per DTC record in response: [dtcHB, dtcMB, dtcLB, statusByte]. */
#define SVC_0x19_BYTES_PER_DTC                        (4U)

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief Append one DTC record to the response buffer.
 *
 * Writes [dtcHB, dtcMB, dtcLB, statusByte] at resp->data[resp->length]
 * and increments resp->length by 4.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buffer capacity exceeded.
 */
static uds_status_t append_dtc_record(
    uds_msg_buf_t *resp,
    uint32_t       dtc_code,
    uint8_t        status_byte)
{
    if (((uint32_t)resp->length + (uint32_t)SVC_0x19_BYTES_PER_DTC)
            > (uint32_t)UDS_MAX_PAYLOAD_LEN) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    resp->data[resp->length]      = (uint8_t)((dtc_code >> 16U) & 0xFFU);
    resp->data[resp->length + 1U] = (uint8_t)((dtc_code >>  8U) & 0xFFU);
    resp->data[resp->length + 2U] = (uint8_t)( dtc_code         & 0xFFU);
    resp->data[resp->length + 3U] = status_byte;
    resp->length += (uint16_t)SVC_0x19_BYTES_PER_DTC;

    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * Sub-function handlers
 * -------------------------------------------------------------------------- */

/**
 * 0x01 — reportNumberOfDTCByStatusMask
 *
 * Request : [0x19, 0x01, statusMask]
 * Response: [0x59, 0x01, availabilityMask, formatID, countHB, countLB]
 */
static uds_status_t handle_report_num_by_status_mask(
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t status;
    uint8_t      status_mask;
    uint16_t     count;

    /* [SID, subFn, statusMask] = 3 bytes minimum */
    status = uds_service_validate_length(req, (uint16_t)3U);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    status_mask = req->data[2];

    status = dtc_database_count_by_status(status_mask, &count);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* Build response header */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DTC_INFO, resp);
    if (status != UDS_STATUS_OK) { return status; }

    resp->data[1] = (uint8_t)SVC_0x19_SUBFN_REPORT_NUM_BY_STATUS_MASK;
    resp->data[2] = (uint8_t)SVC_0x19_STATUS_AVAILABILITY_MASK;
    resp->data[3] = (uint8_t)SVC_0x19_DTC_FORMAT_ISO14229;
    resp->data[4] = (uint8_t)((count >> 8U) & 0xFFU);
    resp->data[5] = (uint8_t)( count        & 0xFFU);
    resp->length  = (uint16_t)6U;

    return UDS_STATUS_OK;
}

/**
 * 0x02 — reportDTCByStatusMask
 *
 * Request : [0x19, 0x02, statusMask]
 * Response: [0x59, 0x02, availabilityMask, {dtc3B, statusByte}...]
 */
static uds_status_t handle_report_dtc_by_status_mask(
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t status;
    uint8_t      status_mask;
    uint16_t     total;
    uint16_t     i;
    uint32_t     dtc_code;
    uint8_t      dtc_status;

    status = uds_service_validate_length(req, (uint16_t)3U);
    if (status != UDS_STATUS_OK) { return status; }

    status_mask = req->data[2];

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DTC_INFO, resp);
    if (status != UDS_STATUS_OK) { return status; }

    resp->data[1] = (uint8_t)SVC_0x19_SUBFN_REPORT_DTC_BY_STATUS_MASK;
    resp->data[2] = (uint8_t)SVC_0x19_STATUS_AVAILABILITY_MASK;
    resp->length  = (uint16_t)3U;

    /* Append all DTCs that match the status mask */
    (void)dtc_database_get_count(&total);

    for (i = (uint16_t)0U; i < total; i++) {
        status = dtc_database_get_by_index(i, &dtc_code, &dtc_status);
        if (status != UDS_STATUS_OK) {
            break;
        }

        /* Include this DTC only if it matches the requested mask */
        if ((status_mask == (uint8_t)0x00U)
                || ((dtc_status & status_mask) != (uint8_t)0U)) {
            status = append_dtc_record(resp, dtc_code, dtc_status);
            if (status != UDS_STATUS_OK) {
                return status; /* Buffer overflow — response too large */
            }
        }
    }

    return UDS_STATUS_OK;
}

/**
 * 0x03 — reportDTCSnapshotIdentification
 *
 * Request : [0x19, 0x03]
 * Response: [0x59, 0x03]  (empty list — no freeze-frame data stored)
 *
 * ISO 14229-1 §13.3.3: an ECU that stores no DTC snapshot records shall
 * respond with an empty list. Tester tools must accept this.
 */
static uds_status_t handle_report_dtc_snapshot_id(
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t status;

    status = uds_service_validate_length(req, (uint16_t)2U);
    if (status != UDS_STATUS_OK) { return status; }

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DTC_INFO, resp);
    if (status != UDS_STATUS_OK) { return status; }

    resp->data[1] = (uint8_t)SVC_0x19_SUBFN_REPORT_DTC_SNAPSHOT_ID;
    resp->length  = (uint16_t)2U;
    /* No snapshot records appended — empty list is ISO-compliant. */

    return UDS_STATUS_OK;
}

/**
 * 0x04 — reportDTCSnapshotRecordByDTCNumber
 *
 * Request : [0x19, 0x04, dtcHB, dtcMB, dtcLB, snapshotRecordNumber]
 * Response: [0x59, 0x04, dtcHB, dtcMB, dtcLB, statusByte,
 *            snapshotRecordNumber, numberOfIdentifiers=0x00]
 *
 * This implementation acknowledges the DTC and record number but returns
 * no freeze-frame data (numberOfIdentifiers = 0). This is ISO-compliant
 * for ECUs that do not implement DTC snapshot storage.
 */
static uds_status_t handle_report_dtc_snapshot_by_dtc(
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t  status;
    uint32_t      dtc_code;
    uint8_t       snapshot_record_num;
    dtc_entry_t  *entry;

    /* [SID, subFn, dtcHB, dtcMB, dtcLB, recordNumber] = 6 bytes */
    status = uds_service_validate_length(req, (uint16_t)6U);
    if (status != UDS_STATUS_OK) { return status; }

    dtc_code            = ((uint32_t)req->data[2] << 16U)
                        | ((uint32_t)req->data[3] <<  8U)
                        |  (uint32_t)req->data[4];
    snapshot_record_num = req->data[5];

    /* Validate that the requested DTC is registered */
    entry = dtc_database_find(dtc_code);
    if (entry == NULL) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DTC_INFO, resp);
    if (status != UDS_STATUS_OK) { return status; }

    resp->data[1] = (uint8_t)SVC_0x19_SUBFN_REPORT_DTC_SNAPSHOT_BY_DTC;
    resp->data[2] = (uint8_t)((dtc_code >> 16U) & 0xFFU);
    resp->data[3] = (uint8_t)((dtc_code >>  8U) & 0xFFU);
    resp->data[4] = (uint8_t)( dtc_code         & 0xFFU);
    resp->data[5] = entry->status_byte;
    resp->data[6] = snapshot_record_num;
    resp->data[7] = (uint8_t)0x00U; /* numberOfIdentifiers = 0 (no freeze-frame) */
    resp->length  = (uint16_t)8U;

    return UDS_STATUS_OK;
}

/**
 * 0x06 — reportDTCExtDataRecordByDTCNumber
 *
 * Request : [0x19, 0x06, dtcHB, dtcMB, dtcLB, extDataRecordNumber]
 * Response: [0x59, 0x06, dtcHB, dtcMB, dtcLB, statusByte,
 *            extDataRecordNumber, recordLength=0x00]
 *
 * Returns the DTC status and an empty extended data record.
 * Extended data (e.g. OccurrenceCounter, AgingCounter) is not stored
 * in this implementation.
 */
static uds_status_t handle_report_dtc_ext_data_by_dtc(
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t  status;
    uint32_t      dtc_code;
    uint8_t       ext_data_record_num;
    dtc_entry_t  *entry;

    /* [SID, subFn, dtcHB, dtcMB, dtcLB, recordNumber] = 6 bytes */
    status = uds_service_validate_length(req, (uint16_t)6U);
    if (status != UDS_STATUS_OK) { return status; }

    dtc_code           = ((uint32_t)req->data[2] << 16U)
                       | ((uint32_t)req->data[3] <<  8U)
                       |  (uint32_t)req->data[4];
    ext_data_record_num = req->data[5];

    entry = dtc_database_find(dtc_code);
    if (entry == NULL) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DTC_INFO, resp);
    if (status != UDS_STATUS_OK) { return status; }

    resp->data[1] = (uint8_t)SVC_0x19_SUBFN_REPORT_DTC_EXT_DATA_BY_DTC;
    resp->data[2] = (uint8_t)((dtc_code >> 16U) & 0xFFU);
    resp->data[3] = (uint8_t)((dtc_code >>  8U) & 0xFFU);
    resp->data[4] = (uint8_t)( dtc_code         & 0xFFU);
    resp->data[5] = entry->status_byte;
    resp->data[6] = ext_data_record_num;
    resp->data[7] = (uint8_t)0x00U; /* recordLength = 0 (no ext data) */
    resp->length  = (uint16_t)8U;

    return UDS_STATUS_OK;
}

/**
 * 0x0A — reportSupportedDTCs
 *
 * Request : [0x19, 0x0A]
 * Response: [0x59, 0x0A, availabilityMask, {dtc3B, statusByte}...]
 *
 * Returns every registered DTC and its current status, regardless of
 * whether any fault bits are set. This is the "read DTC catalogue" function.
 */
static uds_status_t handle_report_supported_dtcs(
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t status;
    uint16_t     total;
    uint16_t     i;
    uint32_t     dtc_code;
    uint8_t      dtc_status;

    status = uds_service_validate_length(req, (uint16_t)2U);
    if (status != UDS_STATUS_OK) { return status; }

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DTC_INFO, resp);
    if (status != UDS_STATUS_OK) { return status; }

    resp->data[1] = (uint8_t)SVC_0x19_SUBFN_REPORT_SUPPORTED_DTCS;
    resp->data[2] = (uint8_t)SVC_0x19_STATUS_AVAILABILITY_MASK;
    resp->length  = (uint16_t)3U;

    (void)dtc_database_get_count(&total);

    for (i = (uint16_t)0U; i < total; i++) {
        status = dtc_database_get_by_index(i, &dtc_code, &dtc_status);
        if (status != UDS_STATUS_OK) {
            break;
        }

        status = append_dtc_record(resp, dtc_code, dtc_status);
        if (status != UDS_STATUS_OK) {
            return status; /* Response too large for buffer */
        }
    }

    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * SID 0x19 dispatcher
 * -------------------------------------------------------------------------- */

/**
 * Top-level 0x19 request router.
 *
 * Validates minimum length, strips suppressPosRspMsgIndicationBit from
 * the sub-function byte, then dispatches to the appropriate sub-function
 * handler.
 *
 * NOTE: 0x19 is listed in services.h with suppress_pos_response_supported=true
 *       because ISO 14229-1 §13.2.4 specifies that all sub-functions of
 *       ReadDTCInformation support the suppress bit.
 */
uds_status_t uds_service_0x19_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t status;
    uint8_t      sub_function;

    (void)ctx; /* server context not needed for routing or access control */

    /* Minimum: [SID, subFunction] = 2 bytes */
    status = uds_service_validate_length(req, (uint16_t)2U);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* Strip suppressPosRspMsgIndicationBit — server dispatcher handles suppress */
    sub_function = (uint8_t)(req->data[1] & (uint8_t)SVC_0x19_SUBFN_MASK);

    switch (sub_function) {
        case SVC_0x19_SUBFN_REPORT_NUM_BY_STATUS_MASK:
            return handle_report_num_by_status_mask(req, resp);

        case SVC_0x19_SUBFN_REPORT_DTC_BY_STATUS_MASK:
            return handle_report_dtc_by_status_mask(req, resp);

        case SVC_0x19_SUBFN_REPORT_DTC_SNAPSHOT_ID:
            return handle_report_dtc_snapshot_id(req, resp);

        case SVC_0x19_SUBFN_REPORT_DTC_SNAPSHOT_BY_DTC:
            return handle_report_dtc_snapshot_by_dtc(req, resp);

        case SVC_0x19_SUBFN_REPORT_DTC_EXT_DATA_BY_DTC:
            return handle_report_dtc_ext_data_by_dtc(req, resp);

        case SVC_0x19_SUBFN_REPORT_SUPPORTED_DTCS:
            return handle_report_supported_dtcs(req, resp);

        default:
            return UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP;
    }
}
