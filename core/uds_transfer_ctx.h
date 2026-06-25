/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_transfer_ctx.h
 *
 * PURPOSE: Shared transfer session state for UDS download services
 *          0x34 (RequestDownload), 0x36 (TransferData),
 *          0x37 (RequestTransferExit).
 *
 * DESIGN:
 *   The three services share a single static transfer_ctx_t that is:
 *     - Initialised by service_0x34 on a valid RequestDownload request.
 *     - Read/updated by service_0x36 on each TransferData block.
 *     - Consumed and reset by service_0x37 on RequestTransferExit.
 *
 *   Only one active transfer is supported at a time.  A new RequestDownload
 *   while a transfer is in progress aborts the current transfer and starts
 *   fresh (ISO 14229-1 §14.3.2 permits this).
 *
 * STATE MACHINE:
 *
 *   IDLE  ─── 0x34 valid ──→  ACTIVE
 *   ACTIVE ── 0x36 valid ──→  ACTIVE   (remains active for each block)
 *   ACTIVE ── 0x37 valid ──→  IDLE     (reset after exit)
 *   ACTIVE ── 0x34 again  ──→  ACTIVE  (abort + restart)
 *   Any state ── ECUReset  ──→  IDLE   (reset clears transfer)
 *
 * CRC ACCUMULATION:
 *   A running CRC-32 (ISO 3309 / MPEG-2 polynomial 0xEDB88320) is maintained
 *   over all transferred payload bytes (excluding block sequence counter byte).
 *   Service 0x37 compares this accumulated value against the
 *   transferRequestParameterRecord CRC if one is supplied.
 *
 * SAFETY:
 *   REQ-DL-001: next_expected_block_seq must wrap at 0xFF → 0x01 (never 0x00).
 *   REQ-DL-002: bytes_remaining must reach exactly 0 before 0x37 is accepted.
 *   REQ-DL-003: transfer_ctx must be reset to IDLE on any abnormal termination.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#ifndef UDS_TRANSFER_CTX_H
#define UDS_TRANSFER_CTX_H

#include "uds_types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Transfer state enumeration
 * -------------------------------------------------------------------------- */

/**
 * @brief States of the firmware download state machine.
 */
typedef enum uds_transfer_state {
    UDS_TRANSFER_IDLE   = 0x00U, /**< No active transfer.                  */
    UDS_TRANSFER_ACTIVE = 0x01U  /**< Transfer initialised, blocks expected.*/
} uds_transfer_state_t;

/* --------------------------------------------------------------------------
 * Transfer direction enumeration
 * -------------------------------------------------------------------------- */

/**
 * @brief Direction of the active transfer session.
 *
 * Set by service_0x34 (download) or service_0x35 (upload) when the transfer
 * context is initialised.  Read by service_0x36 to select the correct data
 * path (write to flash vs. read from flash).
 */
typedef enum uds_transfer_direction {
    UDS_TRANSFER_DIR_DOWNLOAD = 0x00U, /**< 0x34 → 0x36 → 0x37: tester writes to ECU. */
    UDS_TRANSFER_DIR_UPLOAD   = 0x01U  /**< 0x35 → 0x36 → 0x37: ECU reads to tester.  */
} uds_transfer_direction_t;

/* --------------------------------------------------------------------------
 * Transfer context
 * -------------------------------------------------------------------------- */

/**
 * @brief State machine context shared across services 0x34, 0x36, 0x37.
 *
 * One static instance exists in uds_transfer_ctx.c.
 * Access via uds_transfer_ctx_get() only.
 */
typedef struct uds_transfer_ctx {
    uds_transfer_state_t     state;         /**< Current state machine state.   */
    uds_transfer_direction_t direction;     /**< Transfer direction, set by 0x34 or 0x35. */

    uint32_t  target_address;               /**< Base address for this download. */
    uint32_t  total_size_bytes;             /**< Total image size from 0x34.     */
    uint32_t  bytes_remaining;             /**< Bytes not yet written to flash. */
    uint32_t  next_write_address;          /**< Next flash write target address.*/

    uint8_t   next_expected_block_seq;     /**< Wraps 0x01–0xFF; 0x00 invalid.  */

    uint32_t  crc_accumulator;            /**< Running CRC-32 over payload data.*/

    /**
     * @brief Write accumulation buffer.
     *
     * Collects incoming TransferData payload bytes until a full
     * max_block_length chunk is ready for flash_write_cb.
     * Sized to the maximum supported block length.
     */
    uint8_t   write_buf[512U];            /**< Block accumulation buffer (512 B max).*/
    uint16_t  write_buf_fill;             /**< Bytes currently in write_buf[]. */
    uint16_t  write_buf_capacity;        /**< Capacity in use for this session. */

    bool      crc_check_requested;       /**< True if 0x37 must validate CRC.  */
} uds_transfer_ctx_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Reset the transfer context to IDLE state.
 *
 * Called by service_0x37 after a successful transfer exit, by service_0x34
 * when a new download aborts an in-progress one, and by the ECU reset handler.
 *
 * @param[in,out] ctx  Pointer to the transfer context to reset.
 */
void uds_transfer_ctx_reset(uds_transfer_ctx_t *ctx);

/**
 * @brief Retrieve the single global transfer context.
 *
 * Returns a pointer to the statically-allocated context.
 * Never returns NULL.
 *
 * @return Pointer to the global transfer_ctx_t.
 */
uds_transfer_ctx_t *uds_transfer_ctx_get(void);

/**
 * @brief Update the running CRC-32 accumulator.
 *
 * Uses ISO 3309 / MPEG-2 polynomial (0xEDB88320, reflected).
 * Called by service_0x36 for each payload byte of each TransferData block.
 *
 * @param[in] crc_in  Current CRC accumulator value (initialise with 0xFFFFFFFFU).
 * @param[in] data    Pointer to data bytes to feed into CRC.
 * @param[in] length  Number of bytes to process.
 *
 * @return Updated CRC-32 accumulator (NOT finalised — do not XOR with 0xFFFFFFFF
 *         until the full image has been transferred).
 */
uint32_t uds_transfer_crc32_update(uint32_t       crc_in,
                                    const uint8_t *data,
                                    uint32_t       length);

/**
 * @brief Finalise the CRC-32 accumulator.
 *
 * XORs the accumulator with 0xFFFFFFFF to produce the final CRC-32 value.
 * Called once by service_0x37 before comparing with the supplied checksum.
 *
 * @param[in] crc_accumulated  Accumulated CRC value (from uds_transfer_crc32_update).
 *
 * @return Final CRC-32 value.
 */
uint32_t uds_transfer_crc32_finalise(uint32_t crc_accumulated);

#ifdef __cplusplus
}
#endif

#endif /* UDS_TRANSFER_CTX_H */
