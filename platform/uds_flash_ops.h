/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/uds_flash_ops.h
 *
 * PURPOSE: Flash operations abstraction for UDS download services.
 *
 * Provides the flash_ops_t callback table registered at stack init.
 * Service 0x34 (RequestDownload), 0x36 (TransferData), and 0x37
 * (RequestTransferExit) use these callbacks to erase, write, and verify
 * flash memory without depending on any specific flash driver.
 *
 * PLATFORM IMPLEMENTATIONS:
 *   Zephyr  : platform/zephyr_flash_ops.c  — uses flash_area_erase /
 *             flash_area_write targeting the MCUboot secondary slot
 *             (FLASH_AREA_ID(image_1) from <storage/flash_map.h>).
 *   Harness : platform/harness_flash_mock.c — RAM-backed implementation
 *             following the same pattern as nvm_store_mock.c.
 *             Compiled when HARNESS_FLASH_MOCK=1 is defined.
 *
 * DESIGN CONSTRAINTS:
 *   - No dynamic memory allocation.
 *   - All callbacks must complete within a bounded, deterministic time
 *     window compatible with the UDS P2server_max timing constraint.
 *     For large flash operations (erase), the caller is expected to use
 *     NRC 0x78 (requestCorrectlyReceived-ResponsePending) before invoking
 *     the erase callback.  This file defines the interface only; the
 *     response-pending mechanism is handled in service_0x34.c.
 *   - All callbacks receive the full transfer context pointer so they can
 *     update shared state if needed.
 *
 * SAFETY:
 *   REQ-FLASH-001: flash_ops_t must be registered before any 0x34 request
 *                  is accepted.  service_0x34.c rejects requests with NRC
 *                  0x22 (conditionsNotCorrect) if ops is NULL.
 *   REQ-FLASH-002: address + size must be validated against a platform-
 *                  provided memory map before any erase or write.
 *   REQ-FLASH-003: flash_verify_cb must be called after transfer exit to
 *                  confirm CRC integrity before the image is accepted.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#ifndef UDS_FLASH_OPS_H
#define UDS_FLASH_OPS_H

#include "uds_types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Flash memory map descriptor
 *
 * Registered with the flash ops table to constrain which address ranges
 * are permitted for download operations.
 * -------------------------------------------------------------------------- */

/**
 * @brief A single entry in the permissible memory map.
 *
 * Download requests with (address + size) outside all registered regions
 * are rejected with NRC 0x31 (requestOutOfRange).
 */
typedef struct uds_flash_region {
    uint32_t base_address;   /**< Inclusive start of the permitted region. */
    uint32_t size_bytes;     /**< Length in bytes of the permitted region. */
    bool     writable;       /**< True if write operations are allowed here (0x34/0x3D). */
    bool     readable;       /**< True if read operations are allowed here (0x23/0x35). */
} uds_flash_region_t;

/* --------------------------------------------------------------------------
 * Flash operations callback table
 * -------------------------------------------------------------------------- */

/**
 * @brief Callback: erase a flash region before a firmware download.
 *
 * Called once by service_0x34 (RequestDownload) after address validation.
 * Must erase at least [address, address + size_bytes).
 *
 * @param[in] address     Start address of the region to erase.
 * @param[in] size_bytes  Number of bytes to erase.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_PLATFORM on driver failure.
 */
typedef uds_status_t (*uds_flash_erase_cb_fn)(uint32_t address,
                                               uint32_t size_bytes);

/**
 * @brief Callback: write a block of data to flash.
 *
 * Called by service_0x36 (TransferData) each time a complete
 * maxNumberOfBlockLength chunk has been accumulated.
 *
 * @param[in] address    Destination address in flash.
 * @param[in] data       Pointer to data to write.
 * @param[in] length     Number of bytes to write.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_PLATFORM on driver failure.
 */
typedef uds_status_t (*uds_flash_write_cb_fn)(uint32_t       address,
                                               const uint8_t *data,
                                               uint32_t       length);

/**
 * @brief Callback: verify the written image after transfer exit.
 *
 * Called by service_0x37 (RequestTransferExit) to confirm CRC integrity.
 *
 * @param[in]  address       Start address of the image to verify.
 * @param[in]  size_bytes    Size of the image to verify.
 * @param[in]  expected_crc  CRC-32 computed over the received transfer data.
 *
 * @return UDS_STATUS_OK if CRC matches.
 * @return UDS_STATUS_ERR_GENERIC if CRC mismatch.
 * @return UDS_STATUS_ERR_PLATFORM on driver read failure.
 */
typedef uds_status_t (*uds_flash_verify_cb_fn)(uint32_t address,
                                                uint32_t size_bytes,
                                                uint32_t expected_crc);

/**
 * @brief Callback: read a block of data from flash (required for 0x35 RequestUpload).
 *
 * Called by service_0x36 (TransferData) in upload direction to read each block
 * from flash and return it to the tester.
 *
 * @param[in]  address   Source address in flash.
 * @param[out] data      Buffer to receive the read data.
 * @param[in]  length    Number of bytes to read.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_PLATFORM on driver failure.
 */
typedef uds_status_t (*uds_flash_read_cb_fn)(uint32_t  address,
                                              uint8_t  *data,
                                              uint32_t  length);

/**
 * @brief Flash operations table — registered at stack init.
 *
 * erase_cb, write_cb, and verify_cb must be non-NULL when the table is
 * registered for download (0x34) support.  read_cb may be NULL for
 * download-only configurations — service_0x35 checks at request time and
 * returns NRC 0x22 if read_cb is NULL.
 *
 * REQ-FLASH-001: Register before any 0x34 request is accepted.
 */
typedef struct uds_flash_ops {
    uds_flash_erase_cb_fn   erase_cb;        /**< Erase callback.  */
    uds_flash_write_cb_fn   write_cb;        /**< Write callback.  */
    uds_flash_verify_cb_fn  verify_cb;       /**< Verify callback. */
    uds_flash_read_cb_fn    read_cb;         /**< Read callback — required for 0x35 RequestUpload.
                                               *  May be NULL for download-only configurations. */

    const uds_flash_region_t *memory_map;    /**< Array of permitted regions. */
    uint8_t                   region_count;  /**< Number of entries in memory_map[]. */

    /**
     * @brief Maximum number of bytes written per TransferData block.
     *
     * Returned in the RequestDownload positive response as
     * maxNumberOfBlockLength.  Must be <= UDS_MAX_PAYLOAD_LEN - 2
     * (2 bytes for SID + block counter).
     *
     * Recommended: 256 for CAN (limits multi-frame overhead),
     *              4096 for DoIP or CAN FD.
     */
    uint16_t max_block_length;
} uds_flash_ops_t;

/* --------------------------------------------------------------------------
 * Global flash ops registration
 *
 * The single global flash ops pointer is set once at stack init via
 * uds_flash_ops_register().  All three download services read it.
 * -------------------------------------------------------------------------- */

/**
 * @brief Register the flash operations table with the download services.
 *
 * Must be called before any 0x34 request can be processed.
 * Calling again with a non-NULL ops replaces the existing registration.
 * Calling with NULL de-registers (all 0x34 requests will return NRC 0x22).
 *
 * @param[in] ops  Pointer to a statically-allocated flash_ops_t, or NULL.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_INVALID_PARAM if ops->erase_cb, write_cb, or
 *         verify_cb is NULL (when ops itself is non-NULL).  read_cb may
 *         be NULL — download-only configurations do not populate it.
 */
uds_status_t uds_flash_ops_register(const uds_flash_ops_t *ops);

/**
 * @brief Retrieve the currently registered flash ops table.
 *
 * Returns NULL if no table has been registered.
 *
 * @return Pointer to the registered flash_ops_t, or NULL.
 */
const uds_flash_ops_t *uds_flash_ops_get(void);

#ifdef __cplusplus
}
#endif

#endif /* UDS_FLASH_OPS_H */
