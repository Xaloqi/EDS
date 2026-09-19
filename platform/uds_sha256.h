// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/uds_sha256.h
 *
 * PURPOSE: Portable streaming SHA-256 (FIPS 180-4) for DFU image digesting.
 *
 * PHASE 2 — DFU firmware authenticity, hardware-independent half [#232]
 *
 * This module provides a self-contained, streaming SHA-256:
 *
 *     uds_sha256_init()   — reset a context to the FIPS 180-4 initial state
 *     uds_sha256_update() — absorb an arbitrary number of bytes, any number
 *                           of times, at any chunk size
 *     uds_sha256_final()  — pad, length-encode and emit the 32-byte digest
 *
 * WHY STREAMING (issue #232 / issue #282):
 *   The Phase 2 reference image policy computes the digest of the downloaded
 *   firmware image from inside uds_image_policy_t::update_cb, which runs once
 *   per accepted 0x36 TransferData payload range, inside the P2server window.
 *   The server has no NRC 0x78 (responsePending) mechanism with which to buy
 *   itself time at 0x37 RequestTransferExit, so the digest CANNOT be computed
 *   in one pass over a buffered image at transfer exit — there is neither the
 *   RAM to buffer a whole image nor the time budget to hash it in one go.
 *   It must be accumulated incrementally as the blocks arrive.  Chunk sizes
 *   are whatever the tester chose for its block length and are NOT multiples
 *   of the 64-byte SHA-256 block, so the context carries a partial-block
 *   buffer and the API is correct for 1-byte-at-a-time feeding.
 *
 * INTEGRATION:
 *   The hardware-independent half of Phase 2 ships this module UNWIRED:
 *   nothing in core/, transport/ or config/ calls it yet.  The
 *   hardware-dependent half binds it to the MCUboot SHA256 TLV recovered by
 *   platform/uds_mcuboot_image.c.  It lives in platform/ rather than core/
 *   because no crypto primitive may enter core/ — see the boundary
 *   documented in platform/uds_image_policy.h.
 *
 * PORTABILITY:
 *   - No dynamic allocation (no malloc/calloc/realloc/free).
 *   - No Zephyr headers, no external crypto library — compiles on host GCC
 *     for unit tests exactly as core/uds_aes_cmac.c does.
 *   - Fixed-size internal buffers only; every loop is bounded.
 *   - No endianness assumptions: all word<->byte conversion is explicit.
 *
 * PERFORMANCE:
 *   One 64-byte block costs 64 compression rounds.  Roughly 10-20 us per
 *   1 KB TransferData block on a Cortex-M4 at 168 MHz — comfortably inside
 *   the 50 ms P2server window of the reference configuration, which is the
 *   budget uds_image_policy_t::update_cb must respect.
 *
 * SAFETY  : Security-relevant. ASIL-B candidate.
 *           This module has not undergone formal ASIL assessment.
 *           OEM must validate before vehicle deployment.
 *           A digest is an INTEGRITY primitive, not an AUTHENTICITY one: it
 *           proves nothing on its own until compared against a value that
 *           itself arrives under signature.
 * STANDARD: FIPS PUB 180-4, Secure Hash Standard, August 2015.
 *           MISRA C:2012 alignment intended.
 *           Deviation log at bottom of uds_sha256.c.
 * =============================================================================
 */

#ifndef UDS_SHA256_H
#define UDS_SHA256_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Size constants
 * -------------------------------------------------------------------------- */

/** SHA-256 digest size in bytes. */
#define UDS_SHA256_DIGEST_LEN   (32U)

/** SHA-256 compression block size in bytes. */
#define UDS_SHA256_BLOCK_LEN    (64U)

/** Number of 32-bit words in the SHA-256 chaining state (H0..H7). */
#define UDS_SHA256_STATE_WORDS  (8U)

/* --------------------------------------------------------------------------
 * Streaming context
 * -------------------------------------------------------------------------- */

/**
 * @brief Streaming SHA-256 context.
 *
 * Opaque by convention — callers must not read or write the members
 * directly — but the layout is published so that the struct can be placed
 * in static storage or on the caller's stack.  Its total size is the entire
 * RAM cost of digesting an image of any length.
 *
 * SAFETY: The context holds no secret material (SHA-256 is unkeyed), so the
 *         scrub performed by uds_sha256_final() is hygiene rather than a
 *         confidentiality requirement.
 */
typedef struct uds_sha256_ctx {
    uint32_t state[UDS_SHA256_STATE_WORDS];   /**< Chaining value H0..H7.       */
    uint64_t total_len_bits;                  /**< Message length so far, bits. */
    uint8_t  buffer[UDS_SHA256_BLOCK_LEN];    /**< Partial-block accumulator.   */
    size_t   buffer_len;                      /**< Valid bytes in buffer.       */
} uds_sha256_ctx_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialise a context to the FIPS 180-4 SHA-256 initial hash value.
 *
 * Must be called before the first uds_sha256_update() on a context, and
 * again before any reuse of that context for a new message.
 *
 * @param[out] ctx  Context to initialise.
 *
 * @note NULL ctx is a no-op.  See the NULL-argument note on
 *       uds_sha256_update() for why these three entry points check rather
 *       than declaring it the caller's responsibility.
 */
void uds_sha256_init(uds_sha256_ctx_t *ctx);

/**
 * @brief Absorb len bytes into the running digest.
 *
 * May be called any number of times with any chunk sizes, including 0 and 1.
 * The digest produced by uds_sha256_final() depends only on the
 * concatenation of every chunk, never on how the message was split across
 * calls.  This is the property the 0x36 TransferData streaming path relies
 * on, since block sizes are the tester's choice.
 *
 * @param[in,out] ctx   Context previously passed to uds_sha256_init().
 * @param[in]     data  Bytes to absorb.  May be NULL only when len is 0.
 * @param[in]     len   Number of bytes at data.
 *
 * @note NULL-ARGUMENT POLICY: unlike uds_aes128_encrypt_block(), which
 *       documents buffer validity as the caller's responsibility, this
 *       function checks.  The reason is the call site: update() is reached
 *       from a 0x36 handler driven by off-board, attacker-influenced input,
 *       where a defect upstream must degrade into a wrong digest — which
 *       fails the Phase 2 comparison CLOSED — rather than into a memory
 *       fault inside the P2server window.  A rejected chunk is simply not
 *       absorbed; the resulting digest then covers fewer bytes than the
 *       image and cannot match the image's own SHA256 TLV, so the omission
 *       can only ever reject a good image, never accept a bad one.
 */
void uds_sha256_update(uds_sha256_ctx_t *ctx, const uint8_t *data, size_t len);

/**
 * @brief Finalise the digest: apply FIPS 180-4 padding and emit 32 bytes.
 *
 * After this call the context has been scrubbed and MUST be re-initialised
 * with uds_sha256_init() before any further use.  Calling final() twice on
 * one context does NOT yield the same digest twice.
 *
 * @param[in,out] ctx     Context to finalise.
 * @param[out]    digest  Receives the 32-byte big-endian digest.
 *
 * @note NULL ctx or NULL digest is a no-op and leaves digest untouched.  A
 *       caller that ignores this and compares an uninitialised buffer
 *       mismatches, which again fails closed.
 */
void uds_sha256_final(uds_sha256_ctx_t *ctx, uint8_t digest[UDS_SHA256_DIGEST_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* UDS_SHA256_H */
