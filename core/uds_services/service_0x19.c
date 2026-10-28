// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x19.c
 *
 * PURPOSE: SID 0x19 — ReadDTCInformation service handler.
 *
 * ISO 14229-1:2026 §11.3 — ReadDTCInformation
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
 *           returns no records; ISO 14229-1:2026 §11.3.3.1 Table 321: records present only if stored)
 *
 *   0x04 — reportDTCSnapshotRecordByDTCNumber
 *     Request : [0x19, 0x04, dtcHB, dtcMB, dtcLB, snapshotRecordNumber]
 *     Response: [0x59, 0x04, dtcHB, dtcMB, dtcLB, statusByte] for record 0xFF;
 *               any other record number → NRC 0x31 (no snapshots stored) [EDS#451]
 *     Minimal snapshot: no freeze-frame data stored in this implementation.
 *     Returns the DTC + status only (no snapshot records exist).
 *
 *   0x06 — reportDTCExtDataRecordByDTCNumber
 *     Request : [0x19, 0x06, dtcHB, dtcMB, dtcLB, extDataRecordNumber]
 *     Response: [0x59, 0x06, dtcHB, dtcMB, dtcLB, statusByte] for record 0xFF;
 *               any other record number → NRC 0x31 (no extended data) [EDS#451]
 *     Minimal extended data: no extended data stored in this implementation.
 *
 *   0x07 — reportNumberOfDTCBySeverityMaskRecord [EDS#145]
 *     Request : [0x19, 0x07, DTCSeverityMask, DTCStatusMask]
 *     Response: [0x59, 0x07, availabilityMask, fmtID, countHB, countLB]
 *
 *   0x08 — reportDTCBySeverityMaskRecord [EDS#145]
 *     Request : [0x19, 0x08, DTCSeverityMask, DTCStatusMask]
 *     Response: [0x59, 0x08, availabilityMask,
 *                {severity, functionalUnit, dtc3B, statusByte}...]
 *
 *   0x09 — reportSeverityInformationOfDTC [EDS#145]
 *     Request : [0x19, 0x09, dtcHB, dtcMB, dtcLB]
 *     Response: [0x59, 0x09, availabilityMask,
 *                severity, functionalUnit, dtc3B, statusByte]
 *
 *   0x0A — reportSupportedDTCs
 *     Request : [0x19, 0x0A]
 *     Response: [0x59, 0x0A, availabilityMask, {dtc3B, statusByte}...]
 *     Returns all registered DTCs regardless of status.
 *
 *   0x0B/0x0C/0x0D/0x0E — reportFirstTestFailedDTC, reportFirstConfirmedDTC,
 *     reportMostRecentTestFailedDTC, reportMostRecentConfirmedDTC [EDS#145]
 *     Request : [0x19, subFn]
 *     Response: [0x59, subFn, availabilityMask, {dtc3B, statusByte}?]
 *     The record is kept by config/dtc_database.c (dtc_record_t) and
 *     persisted to NVM with the status mirror.
 *
 *   0x14 — reportDTCFaultDetectionCounter (ISO 14229-1:2026 §11.3.1.14, Table 317)
 *     Request : [0x19, 0x14]
 *     Response: [0x59, 0x14, {dtc3B, faultDetectionCounter}...]
 *     Returns pre-confirmed (testFailed==0) DTCs with counter < 0xFF.
 *     No DTCStatusAvailabilityMask byte in this response.
 *
 *   0x15 — reportDTCWithPermanentStatus (ISO 14229-1:2026 §11.3.1.15)
 *     Request : [0x19, 0x15]
 *     Response: [0x59, 0x15, availabilityMask, {dtc3B, statusByte}...]
 *     Returns DTCs marked permanent (is_permanent == true).
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
 * CITATIONS FOR 0x07-0x0E [EDS#145]: checked against ISO 14229-1:2013, the
 * edition available when they were written, and cited with that edition's
 * clause and table numbers. Not yet checked against the 2026 text.
 *
 * SESSION REQUIREMENT (ISO 14229-1:2026 §9.2.1 Table 22):
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
 * Sub-function identifiers (ISO 14229-1:2026 §11.3.2.2 Table 317)
 * -------------------------------------------------------------------------- */
#define SVC_0x19_SUBFN_REPORT_NUM_BY_STATUS_MASK     (0x01U)
#define SVC_0x19_SUBFN_REPORT_DTC_BY_STATUS_MASK     (0x02U)
#define SVC_0x19_SUBFN_REPORT_DTC_SNAPSHOT_ID        (0x03U)
#define SVC_0x19_SUBFN_REPORT_DTC_SNAPSHOT_BY_DTC    (0x04U)
#define SVC_0x19_SUBFN_REPORT_DTC_EXT_DATA_BY_DTC    (0x06U)
#define SVC_0x19_SUBFN_REPORT_NUM_BY_SEVERITY_MASK   (0x07U)
#define SVC_0x19_SUBFN_REPORT_DTC_BY_SEVERITY_MASK   (0x08U)
#define SVC_0x19_SUBFN_REPORT_SEVERITY_INFO_OF_DTC   (0x09U)
#define SVC_0x19_SUBFN_REPORT_SUPPORTED_DTCS         (0x0AU)
#define SVC_0x19_SUBFN_REPORT_FIRST_TEST_FAILED      (0x0BU)
#define SVC_0x19_SUBFN_REPORT_FIRST_CONFIRMED        (0x0CU)
#define SVC_0x19_SUBFN_REPORT_MOST_RECENT_TEST_FAILED (0x0DU)
#define SVC_0x19_SUBFN_REPORT_MOST_RECENT_CONFIRMED  (0x0EU)
#define SVC_0x19_SUBFN_REPORT_FAULT_DETECTION_CTR    (0x14U)
#define SVC_0x19_SUBFN_REPORT_DTC_PERMANENT_STATUS   (0x15U)

/** suppressPosRspMsgIndicationBit mask for sub-function byte. */
#define SVC_0x19_SUPPRESS_BIT                         (0x80U)

/** Mask for actual sub-function value (bits 6..0). */
#define SVC_0x19_SUBFN_MASK                           (0x7FU)

/**
 * @brief DTCStatusAvailabilityMask: the status bits this ECU maintains.
 *
 * [EDS#451] Configurable (CONFIG_UDS_DTC_STATUS_AVAILABILITY_MASK, or
 * -DUDS_DTC_STATUS_AVAILABILITY_MASK=...) because only the integration knows
 * which bits its monitors maintain; default 0xFF (all eight). Request masks
 * are processed using only these bits (ISO 14229-1:2026 Table 318).
 */
#if !defined(UDS_DTC_STATUS_AVAILABILITY_MASK) && defined(CONFIG_UDS_DTC_STATUS_AVAILABILITY_MASK)
#define UDS_DTC_STATUS_AVAILABILITY_MASK  (CONFIG_UDS_DTC_STATUS_AVAILABILITY_MASK)
#endif
#ifndef UDS_DTC_STATUS_AVAILABILITY_MASK
#define UDS_DTC_STATUS_AVAILABILITY_MASK  (0xFFU)
#endif
#define SVC_0x19_STATUS_AVAILABILITY_MASK ((uint8_t)UDS_DTC_STATUS_AVAILABILITY_MASK)

/**
 * @brief DTC format identifier (ISO 14229-1:2026 §11.3.3.1 Table 319).
 * 0x01 = ISO 14229-1 DTC format.
 */
#define SVC_0x19_DTC_FORMAT_ISO14229                  (0x01U)

/** Bytes per DTC record in response: [dtcHB, dtcMB, dtcLB, statusByte]. */
#define SVC_0x19_BYTES_PER_DTC                        (4U)

/** [EDS#451] reportDTCFaultDetectionCounter reports DTCs whose signed counter
 *  is 1..126 (0x01..0x7E): ISO 14229-1:2026 Table 317 (0x14); +127 means
 *  "failed" and is not reported. */
#define SVC_0x19_FDC_MAX_PREFAILED                    (126)

/** [EDS#145] DTCFunctionalUnit: "implementation specific" (ISO 14229-1:2013
 *  §11.3.3.2, DTCAndSeverityRecord). EDS has no per-DTC field for it yet, so
 *  every DTC reports 0x00. */
#define SVC_0x19_DTC_FUNCTIONAL_UNIT                  (0x00U)

/** [EDS#145] Bytes per DTCAndSeverityRecord: severity, FU, dtc3B, status. */
#define SVC_0x19_BYTES_PER_SEVERITY_RECORD            (6U)

/** [EDS#451] "All records" DTCSnapshot / DTCExtendedData record numbers. */
#define SVC_0x19_RECORD_ALL                           (0xFFU)

/* [EDS#451] Every implemented sub-function has a fixed request length
 * (ISO 14229-1:2026 Tables 302-308); a longer or shorter one is NRC 0x13.
 * uds_service_validate_length() is a minimum check only. */
static uds_status_t s_exact_len(const uds_msg_buf_t *req, uint16_t len)
{
    if (req == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    return (req->length == len) ? UDS_STATUS_OK : UDS_STATUS_ERR_INVALID_PARAM;
}

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
    status = s_exact_len(req, (uint16_t)3U);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    status_mask = req->data[2];

    /* Only the bits this ECU supports are processed (Table 318). */
    status = dtc_database_count_by_status(
        (uint8_t)(status_mask & SVC_0x19_STATUS_AVAILABILITY_MASK), &count);
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

    status = s_exact_len(req, (uint16_t)3U);
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

        /* [EDS#451] Match = (statusOfDTC & DTCStatusMask) != 0, using only the
         * supported bits (§11.3.1.3, Table 318). A mask of 0x00 matches
         * nothing; it used to return every DTC. */
        if ((dtc_status & status_mask & SVC_0x19_STATUS_AVAILABILITY_MASK) != (uint8_t)0U) {
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
 * ISO 14229-1:2026 §11.3.1.4 / Table 321: DTCRecord and DTCSnapshotRecordNumber are
 * present only if a snapshot record is stored, so an ECU with none returns
 * the positive response without records.
 */
static uds_status_t handle_report_dtc_snapshot_id(
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t status;

    status = s_exact_len(req, (uint16_t)2U);
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
 * Response: [0x59, 0x04, dtcHB, dtcMB, dtcLB, statusByte]   (record 0xFF only)
 *
 * No snapshot records are stored, so record 0xFF ("all") returns the
 * DTCAndStatusRecord alone and any specific record number is unsupported
 * (NRC 0x31), per ISO 14229-1:2026 §11.3.1.5 [EDS#451].
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
    status = s_exact_len(req, (uint16_t)6U);
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

    /* [EDS#451] ISO 14229-1:2026 §11.3.1.5: an unsupported
     * DTCSnapshotRecordNumber is a negative response. EDS stores no snapshot
     * records, so only 0xFF ("all records") is valid, and with none stored the
     * answer is the DTCAndStatusRecord alone (Table 322: record fields are
     * present only if a record is available). It used to accept any number
     * and append a fabricated empty record. */
    if (snapshot_record_num != (uint8_t)SVC_0x19_RECORD_ALL) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DTC_INFO, resp);
    if (status != UDS_STATUS_OK) { return status; }

    resp->data[1] = (uint8_t)SVC_0x19_SUBFN_REPORT_DTC_SNAPSHOT_BY_DTC;
    resp->data[2] = (uint8_t)((dtc_code >> 16U) & 0xFFU);
    resp->data[3] = (uint8_t)((dtc_code >>  8U) & 0xFFU);
    resp->data[4] = (uint8_t)( dtc_code         & 0xFFU);
    resp->data[5] = entry->status_byte;
    resp->length  = (uint16_t)6U;

    return UDS_STATUS_OK;
}

/**
 * 0x06 — reportDTCExtDataRecordByDTCNumber
 *
 * Request : [0x19, 0x06, dtcHB, dtcMB, dtcLB, extDataRecordNumber]
 * Response: [0x59, 0x06, dtcHB, dtcMB, dtcLB, statusByte]   (record 0xFF only)
 *
 * Extended data (e.g. OccurrenceCounter, AgingCounter) is not stored, so
 * record 0xFF ("all") returns the DTCAndStatusRecord alone and any other
 * record number, 0xFE included, is NRC 0x31 (§11.3.1.7) [EDS#451].
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
    status = s_exact_len(req, (uint16_t)6U);
    if (status != UDS_STATUS_OK) { return status; }

    dtc_code           = ((uint32_t)req->data[2] << 16U)
                       | ((uint32_t)req->data[3] <<  8U)
                       |  (uint32_t)req->data[4];
    ext_data_record_num = req->data[5];

    entry = dtc_database_find(dtc_code);
    if (entry == NULL) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /* [EDS#451] ISO 14229-1:2026 §11.3.1.7: an unsupported
     * UserDefDTCExtDataRecordNumber is a negative response, including 0xFE
     * when no OBD extended data records are supported. EDS stores none, so
     * only 0xFF ("all records") is valid, answered with the DTCAndStatusRecord
     * alone (Table 324). It used to append a fabricated "recordLength = 0". */
    if (ext_data_record_num != (uint8_t)SVC_0x19_RECORD_ALL) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DTC_INFO, resp);
    if (status != UDS_STATUS_OK) { return status; }

    resp->data[1] = (uint8_t)SVC_0x19_SUBFN_REPORT_DTC_EXT_DATA_BY_DTC;
    resp->data[2] = (uint8_t)((dtc_code >> 16U) & 0xFFU);
    resp->data[3] = (uint8_t)((dtc_code >>  8U) & 0xFFU);
    resp->data[4] = (uint8_t)( dtc_code         & 0xFFU);
    resp->data[5] = entry->status_byte;
    resp->length  = (uint16_t)6U;

    return UDS_STATUS_OK;
}

/**
 * 0x07 — reportNumberOfDTCBySeverityMaskRecord [EDS#145]
 * 0x08 — reportDTCBySeverityMaskRecord          [EDS#145]
 *
 * Request : [0x19, subFn, DTCSeverityMask, DTCStatusMask]   (Table 260)
 * Response 0x07: [0x59, 0x07, availabilityMask, formatID, countHB, countLB]
 *                (Table 271)
 * Response 0x08: [0x59, 0x08, availabilityMask,
 *                 {severity, functionalUnit, dtc3B, statusByte}...]  (Table 277)
 *
 * ISO 14229-1:2013 §11.3.1.8 (0x07) and §11.3.1.9 (0x08): a DTC matches if
 * ((statusOfDTC & DTCStatusMask) != 0) && ((DTCSeverity & DTCSeverityMask)
 * != 0), using only the status bits the server supports. No match: count 0
 * (0x07) or no records after the availability mask (0x08).
 */
static uds_status_t handle_report_by_severity_mask(
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp,
    uint8_t              sub_function)
{
    uds_status_t  status;
    uint8_t       severity_mask;
    uint8_t       status_mask;
    uint16_t      total;
    uint16_t      i;
    uint16_t      count;
    uint32_t      dtc_code;
    uint8_t       dtc_status;
    dtc_entry_t  *entry;

    status = s_exact_len(req, (uint16_t)4U);
    if (status != UDS_STATUS_OK) { return status; }

    severity_mask = req->data[2];
    status_mask   = (uint8_t)(req->data[3] & SVC_0x19_STATUS_AVAILABILITY_MASK);

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DTC_INFO, resp);
    if (status != UDS_STATUS_OK) { return status; }

    resp->data[1] = sub_function;
    resp->data[2] = (uint8_t)SVC_0x19_STATUS_AVAILABILITY_MASK;
    resp->length  = (uint16_t)3U;

    count = (uint16_t)0U;
    (void)dtc_database_get_count(&total);

    for (i = (uint16_t)0U; i < total; i++) {
        status = dtc_database_get_by_index(i, &dtc_code, &dtc_status);
        if (status != UDS_STATUS_OK) {
            break;
        }
        entry = dtc_database_find(dtc_code);
        if (entry == NULL) {
            continue;
        }
        if (((dtc_status & status_mask) == (uint8_t)0U)
                || ((entry->severity & severity_mask) == (uint8_t)0U)) {
            continue;
        }

        if (sub_function == (uint8_t)SVC_0x19_SUBFN_REPORT_NUM_BY_SEVERITY_MASK) {
            count++;
        } else {
            if (((uint32_t)resp->length + (uint32_t)SVC_0x19_BYTES_PER_SEVERITY_RECORD)
                    > (uint32_t)UDS_MAX_PAYLOAD_LEN) {
                return UDS_STATUS_ERR_BUFFER_OVERFLOW;
            }
            resp->data[resp->length] = entry->severity;
            resp->data[resp->length + 1U] = (uint8_t)SVC_0x19_DTC_FUNCTIONAL_UNIT;
            resp->length += (uint16_t)2U;
            status = append_dtc_record(resp, dtc_code, dtc_status);
            if (status != UDS_STATUS_OK) {
                return status;
            }
        }
    }

    if (sub_function == (uint8_t)SVC_0x19_SUBFN_REPORT_NUM_BY_SEVERITY_MASK) {
        resp->data[3] = (uint8_t)SVC_0x19_DTC_FORMAT_ISO14229;
        resp->data[4] = (uint8_t)((count >> 8U) & 0xFFU);
        resp->data[5] = (uint8_t)( count        & 0xFFU);
        resp->length  = (uint16_t)6U;
    }

    return UDS_STATUS_OK;
}

/**
 * 0x09 — reportSeverityInformationOfDTC [EDS#145]
 *
 * Request : [0x19, 0x09, dtcHB, dtcMB, dtcLB]                   (Table 261)
 * Response: [0x59, 0x09, availabilityMask,
 *            severity, functionalUnit, dtcHB, dtcMB, dtcLB, statusByte]
 *                                                               (Table 277)
 *
 * ISO 14229-1:2013 §11.3.1.10: exact match on the DTC number. A DTC the
 * server does not support is NRC 0x31 (Table 286: "a DTCMaskRecord that was
 * not recognized"), as 0x04 and 0x06 already answer.
 */
static uds_status_t handle_report_severity_info_of_dtc(
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t  status;
    uint32_t      dtc_code;
    dtc_entry_t  *entry;

    status = s_exact_len(req, (uint16_t)5U);
    if (status != UDS_STATUS_OK) { return status; }

    dtc_code = ((uint32_t)req->data[2] << 16U)
             | ((uint32_t)req->data[3] <<  8U)
             |  (uint32_t)req->data[4];

    entry = dtc_database_find(dtc_code);
    if (entry == NULL) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DTC_INFO, resp);
    if (status != UDS_STATUS_OK) { return status; }

    resp->data[1] = (uint8_t)SVC_0x19_SUBFN_REPORT_SEVERITY_INFO_OF_DTC;
    resp->data[2] = (uint8_t)SVC_0x19_STATUS_AVAILABILITY_MASK;
    resp->data[3] = entry->severity;
    resp->data[4] = (uint8_t)SVC_0x19_DTC_FUNCTIONAL_UNIT;
    resp->length  = (uint16_t)5U;

    return append_dtc_record(resp, dtc_code, entry->status_byte);
}

/**
 * 0x0B — reportFirstTestFailedDTC        [EDS#145]
 * 0x0C — reportFirstConfirmedDTC
 * 0x0D — reportMostRecentTestFailedDTC
 * 0x0E — reportMostRecentConfirmedDTC
 *
 * Request : [0x19, subFn]                                       (Table 262)
 * Response: [0x59, subFn, availabilityMask, {dtc3B, statusByte}?] (Table 272)
 *
 * ISO 14229-1:2013 §11.3.1.12 (0x0B/0x0D) and §11.3.1.13 (0x0C/0x0E): the
 * recorded DTC with its current status; nothing after the availability mask
 * if no DTC has failed / been confirmed since the last clear. The record is
 * independent of aging, so it is reported whatever the DTC's status now is.
 */
static uds_status_t handle_report_first_or_recent(
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp,
    uint8_t              sub_function,
    dtc_record_t         which)
{
    uds_status_t  status;
    uint32_t      dtc_code;
    dtc_entry_t  *entry;

    status = s_exact_len(req, (uint16_t)2U);
    if (status != UDS_STATUS_OK) { return status; }

    dtc_code = 0U;
    status = dtc_database_get_record(which, &dtc_code);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DTC_INFO, resp);
    if (status != UDS_STATUS_OK) { return status; }

    resp->data[1] = sub_function;
    resp->data[2] = (uint8_t)SVC_0x19_STATUS_AVAILABILITY_MASK;
    resp->length  = (uint16_t)3U;

    entry = (dtc_code == 0U) ? NULL : dtc_database_find(dtc_code);
    if (entry == NULL) {
        return UDS_STATUS_OK;
    }
    return append_dtc_record(resp, dtc_code, entry->status_byte);
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

    status = s_exact_len(req, (uint16_t)2U);
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

/**
 * 0x14 — reportDTCFaultDetectionCounter
 *
 * Request : [0x19, 0x14]
 * Response: [0x59, 0x14, {dtcHB, dtcMB, dtcLB, faultDetectionCounter}...]
 *
 * ISO 14229-1:2026 §11.3.1.14, Table 317.
 *
 * Returns a 4-byte record for each DTC whose signed fault-detection counter
 * is 1..126 (Table 317) [EDS#451].
 *
 * NOTE: No DTCStatusAvailabilityMask byte in this response — ISO 14229-1
 *       §11.3.20 response format differs from 0x01/0x02/0x0A.
 */
static uds_status_t handle_report_fault_detection_counter(
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t  status;
    uint16_t      total;
    uint16_t      i;
    uint32_t      dtc_code;
    uint8_t       dtc_status;
    dtc_entry_t  *entry;

    status = s_exact_len(req, (uint16_t)2U);
    if (status != UDS_STATUS_OK) { return status; }

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DTC_INFO, resp);
    if (status != UDS_STATUS_OK) { return status; }

    resp->data[1] = (uint8_t)SVC_0x19_SUBFN_REPORT_FAULT_DETECTION_CTR;
    resp->length  = (uint16_t)2U;

    (void)dtc_database_get_count(&total);

    for (i = (uint16_t)0U; i < total; i++) {
        status = dtc_database_get_by_index(i, &dtc_code, &dtc_status);
        if (status != UDS_STATUS_OK) {
            break;
        }

        (void)dtc_status;
        entry = dtc_database_find(dtc_code);
        if (entry == NULL) {
            continue;
        }

        /* [EDS#451] Table 317 (0x14): report a DTC iff its signed counter is
         * 1..126, "whether or not it has been detected as pending or
         * confirmed". It used to skip every testFailed DTC and exclude only
         * 0xFF, so it reported 0x00, +127 (failed) and negative counters. */
        {
            int8_t fdc = (int8_t)entry->fault_detection_counter;
            if ((fdc < (int8_t)1) || (fdc > (int8_t)SVC_0x19_FDC_MAX_PREFAILED)) {
                continue;
            }
        }

        /* Reuse append_dtc_record: 4th byte is faultDetectionCounter, not status */
        status = append_dtc_record(resp, dtc_code, entry->fault_detection_counter);
        if (status != UDS_STATUS_OK) {
            return status;
        }
    }

    return UDS_STATUS_OK;
}

/**
 * 0x15 — reportDTCWithPermanentStatus
 *
 * Request : [0x19, 0x15]
 * Response: [0x59, 0x15, availabilityMask, {dtcHB, dtcMB, dtcLB, statusByte}...]
 *
 * ISO 14229-1:2026 §11.3.1.15.
 *
 * Returns all DTCs marked permanent (is_permanent == true). These DTCs
 * are not clearable by SID 0x14 — only by application-driven drive-cycle
 * healing via dtc_database_set_permanent(dtc_code, false).
 */
static uds_status_t handle_report_dtc_permanent_status(
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t  status;
    uint16_t      total;
    uint16_t      i;
    uint32_t      dtc_code;
    uint8_t       dtc_status;
    dtc_entry_t  *entry;

    status = s_exact_len(req, (uint16_t)2U);
    if (status != UDS_STATUS_OK) { return status; }

    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DTC_INFO, resp);
    if (status != UDS_STATUS_OK) { return status; }

    resp->data[1] = (uint8_t)SVC_0x19_SUBFN_REPORT_DTC_PERMANENT_STATUS;
    resp->data[2] = (uint8_t)SVC_0x19_STATUS_AVAILABILITY_MASK;
    resp->length  = (uint16_t)3U;

    (void)dtc_database_get_count(&total);

    for (i = (uint16_t)0U; i < total; i++) {
        status = dtc_database_get_by_index(i, &dtc_code, &dtc_status);
        if (status != UDS_STATUS_OK) {
            break;
        }

        entry = dtc_database_find(dtc_code);
        if (entry == NULL) {
            continue;
        }

        if (!entry->is_permanent) {
            continue;
        }

        status = append_dtc_record(resp, dtc_code, dtc_status);
        if (status != UDS_STATUS_OK) {
            return status;
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
 *       because ReadDTCInformation uses a SubFunction parameter and so the
 *       suppress bit (ISO 14229-1:2026 §7.7.3, §11.3.2.2).
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

        case SVC_0x19_SUBFN_REPORT_NUM_BY_SEVERITY_MASK:
        case SVC_0x19_SUBFN_REPORT_DTC_BY_SEVERITY_MASK:
            return handle_report_by_severity_mask(req, resp, sub_function);

        case SVC_0x19_SUBFN_REPORT_SEVERITY_INFO_OF_DTC:
            return handle_report_severity_info_of_dtc(req, resp);

        case SVC_0x19_SUBFN_REPORT_SUPPORTED_DTCS:
            return handle_report_supported_dtcs(req, resp);

        case SVC_0x19_SUBFN_REPORT_FIRST_TEST_FAILED:
            return handle_report_first_or_recent(req, resp, sub_function,
                                                 DTC_RECORD_FIRST_TEST_FAILED);

        case SVC_0x19_SUBFN_REPORT_FIRST_CONFIRMED:
            return handle_report_first_or_recent(req, resp, sub_function,
                                                 DTC_RECORD_FIRST_CONFIRMED);

        case SVC_0x19_SUBFN_REPORT_MOST_RECENT_TEST_FAILED:
            return handle_report_first_or_recent(req, resp, sub_function,
                                                 DTC_RECORD_MOST_RECENT_TEST_FAILED);

        case SVC_0x19_SUBFN_REPORT_MOST_RECENT_CONFIRMED:
            return handle_report_first_or_recent(req, resp, sub_function,
                                                 DTC_RECORD_MOST_RECENT_CONFIRMED);

        case SVC_0x19_SUBFN_REPORT_FAULT_DETECTION_CTR:
            return handle_report_fault_detection_counter(req, resp);

        case SVC_0x19_SUBFN_REPORT_DTC_PERMANENT_STATUS:
            return handle_report_dtc_permanent_status(req, resp);

        default:
            return UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP;
    }
}
