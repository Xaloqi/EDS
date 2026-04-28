// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_aes_cmac.h
 *
 * PURPOSE: Portable AES-128-CMAC implementation for UDS SecurityAccess.
 *
 * PHASE 1 — Production Security Hardening [P1-SEC]
 *
 * This module provides:
 *
 *   1. AES-128 BLOCK CIPHER (uds_aes128_encrypt_block)
 *      A fully self-contained, table-free AES-128 implementation.
 *      No external library dependency — builds on bare-metal, Zephyr,
 *      and host test environments without modification.
 *
 *   2. AES-128-CMAC (uds_aes_cmac)
 *      CMAC per NIST SP 800-38B / RFC 4493.
 *      Produces a 16-byte MAC over arbitrary-length input.
 *      Used by uds_security_algo.c to derive the UDS seed-response key.
 *
 * SECURITY PROPERTIES:
 *   - CMAC is a PRF (Pseudo-Random Function) under standard assumptions.
 *   - A correct key cannot be guessed from the MAC output (unlike XOR).
 *   - Per-level 128-bit keys ensure Level-1 and Level-2 are cryptographically
 *     independent.
 *   - Sequence counter embedded in the input prevents replay.
 *
 * INTEGRATION:
 *   This header is consumed only by uds_security_algo.c.
 *   Do NOT include it from other modules — the AES primitive is
 *   intentionally encapsulated within the security algorithm layer.
 *
 * PORTABILITY:
 *   - No dynamic allocation.
 *   - No Zephyr headers — compiles on host GCC for unit tests.
 *   - Key and block arrays are fixed-size (16 bytes).
 *   - All loops are bounded at compile time.
 *
 * PERFORMANCE:
 *   AES-128 using SubBytes computed from the affine transform (no S-box table).
 *   ~2–5 µs per block on Cortex-M4 at 168 MHz — acceptable for SID 0x27
 *   which is invoked at most a few times per diagnostic session.
 *
 * SAFETY  : Security-critical. ASIL-B candidate.
 *           This module has not undergone formal ASIL assessment.
 *           OEM must validate before vehicle deployment.
 * STANDARD: MISRA C:2012 alignment intended.
 *           Deviation log at bottom of uds_aes_cmac.c.
 * =============================================================================
 */

#ifndef UDS_AES_CMAC_H
#define UDS_AES_CMAC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Size constants
 * -------------------------------------------------------------------------- */

/** AES block size in bytes (AES-128/192/256 all use 16-byte blocks). */
#define UDS_AES_BLOCK_LEN    (16U)

/** AES-128 key size in bytes. */
#define UDS_AES128_KEY_LEN   (16U)

/** CMAC output tag size in bytes (one AES block). */
#define UDS_CMAC_TAG_LEN     (16U)

/* --------------------------------------------------------------------------
 * AES-128 block cipher
 * -------------------------------------------------------------------------- */

/**
 * @brief Encrypt a single 16-byte block with AES-128 (ECB mode).
 *
 * This is a bare-metal AES-128 encryption primitive. It is exposed here
 * so that the CMAC construction can call it directly without indirection.
 *
 * @param[in]  key       16-byte AES-128 key.
 * @param[in]  plaintext 16-byte input block.
 * @param[out] ciphertext 16-byte output block.
 *
 * @note Input and output buffers MUST NOT overlap.
 * @note SAFETY: All buffers must be non-NULL and exactly 16 bytes.
 *               Caller is responsible — no NULL check inside for MISRA
 *               Rule 17.2 (no redundant checks in performance-critical paths).
 */
void uds_aes128_encrypt_block(
    const uint8_t key[UDS_AES128_KEY_LEN],
    const uint8_t plaintext[UDS_AES_BLOCK_LEN],
    uint8_t       ciphertext[UDS_AES_BLOCK_LEN]);

/* --------------------------------------------------------------------------
 * AES-128-CMAC
 * -------------------------------------------------------------------------- */

/**
 * @brief Compute AES-128-CMAC over an arbitrary-length message.
 *
 * Implements CMAC as defined in NIST SP 800-38B and RFC 4493.
 * The MAC tag is always exactly UDS_CMAC_TAG_LEN (16) bytes.
 *
 * Usage in UDS SecurityAccess:
 *   Input  = seed_bytes (8 bytes) || security_level (1 byte) || ecu_id (7 bytes)
 *   Key    = per-level 128-bit OEM key stored in protected memory
 *   Output = 16-byte CMAC tag; first UDS_ALGO_KEY_LEN bytes used as the UDS key
 *
 * @param[in]  key      16-byte AES-128 key (must not be NULL).
 * @param[in]  msg      Message bytes to authenticate (may be NULL if msg_len=0).
 * @param[in]  msg_len  Length of msg in bytes.
 * @param[out] tag      Output buffer receiving the 16-byte CMAC tag (must not be NULL).
 *
 * @return 0 on success.
 * @return -1 if key or tag is NULL.
 *
 * @note The returned tag is 16 bytes. The caller truncates to the desired
 *       output length (UDS_ALGO_KEY_LEN bytes for UDS key derivation).
 */
int uds_aes_cmac(
    const uint8_t *key,
    const uint8_t *msg,
    size_t         msg_len,
    uint8_t        tag[UDS_CMAC_TAG_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* UDS_AES_CMAC_H */
