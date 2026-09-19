// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/uds_mcuboot_image.h
 *
 * PURPOSE: Pure-logic parser for the MCUboot image header and TLV trailer.
 *
 * PHASE 2 — DFU firmware authenticity, hardware-independent half [#232]
 *
 * This module decodes the two on-flash structures an MCUboot-signed image
 * carries around its payload:
 *
 *   1. THE IMAGE HEADER — a 32-byte little-endian record at offset 0 of the
 *      image, giving the magic, load address, header size, protected-TLV
 *      size, body size, flags and version.
 *      -> uds_mcuboot_parse_header()
 *
 *   2. THE TLV AREA(S) — a 4-byte image_tlv_info record followed by a chain
 *      of {type, length, value} entries, sitting immediately after the image
 *      body.  This is where the image's own SHA-256 digest and its signature
 *      live.
 *      -> uds_mcuboot_tlv_iter_begin() / uds_mcuboot_tlv_iter_next()
 *
 * WIRE FORMAT (mirrors MCUboot boot/bootutil/include/bootutil/image.h):
 *
 *   image_header, 32 bytes, all multi-byte fields LITTLE-ENDIAN:
 *     off  0  uint32  ih_magic
 *     off  4  uint32  ih_load_addr
 *     off  8  uint16  ih_hdr_size           size of this header
 *     off 10  uint16  ih_protect_tlv_size   size of the PROTECTED TLV area
 *     off 12  uint32  ih_img_size           body size, header NOT included
 *     off 16  uint32  ih_flags
 *     off 20  uint8   ih_ver.iv_major
 *     off 21  uint8   ih_ver.iv_minor
 *     off 22  uint16  ih_ver.iv_revision
 *     off 24  uint32  ih_ver.iv_build_num
 *     off 28  uint32  _pad1                 reserved, ignored here
 *
 *   image_tlv_info, 4 bytes, starts every TLV area:
 *     off  0  uint16  it_magic    UDS_MCUBOOT_TLV_INFO_MAGIC, or
 *                                 UDS_MCUBOOT_TLV_PROT_INFO_MAGIC for the
 *                                 protected area
 *     off  2  uint16  it_tlv_tot  total area size INCLUDING these 4 bytes
 *
 *   image_tlv, 4 bytes, precedes each entry's value:
 *     off  0  uint16  it_type
 *     off  2  uint16  it_len      length of the VALUE only
 *
 *   Layout after the body:
 *     [tlv_info][tlv hdr #1][value #1][tlv hdr #2][value #2] ... until
 *     it_tlv_tot bytes have been consumed.  A PROTECTED area (magic
 *     ...PROT_INFO_MAGIC, size ih_protect_tlv_size, 0 if absent) may
 *     immediately precede the regular one.
 *
 * SCOPE — WHAT THIS MODULE DELIBERATELY DOES NOT DO:
 *   It has no application-level knowledge.  It does not know which TLV types
 *   an image is required to carry, it does not compute or check the digest,
 *   and it does not verify any signature.  It answers exactly two questions:
 *   "is this byte range structurally a well-formed MCUboot header / TLV
 *   area?" and "what does it contain?".  Deciding that a SHA256 TLV must be
 *   present, that its value must equal the streamed uds_sha256 digest, and
 *   that an ECDSA_SIG TLV must verify under the trust anchor is the job of
 *   the Phase 2 policy that consumes this module.
 *
 *   Choosing WHICH area to iterate — protected or regular — is likewise the
 *   caller's decision: it passes the byte range and the it_magic it expects.
 *
 * DEFENSIVE PARSING CONTRACT:
 *   Flash-read buffers carry no alignment guarantee, and their contents are
 *   attacker-influenced (they arrived over 0x36 TransferData).  This module
 *   therefore NEVER casts a raw buffer to a struct pointer: every multi-byte
 *   field is assembled byte by byte with explicit shifts.  That removes both
 *   the unaligned-access hazard and MISRA C:2012 Rule 11.3.  Every length is
 *   checked against what actually remains in the buffer, using subtraction
 *   on already-validated bounds so that no addition can overflow, and a
 *   corrupt it_len can therefore never produce a pointer or an offset past
 *   the end of the area.
 *
 * INTEGRATION:
 *   The hardware-independent half of Phase 2 ships this module UNWIRED:
 *   nothing in core/, transport/ or config/ calls it yet.
 *
 * PORTABILITY:
 *   - No dynamic allocation (no malloc/calloc/realloc/free).
 *   - No Zephyr headers, no MCUboot headers — the wire format above is
 *     transcribed, not included, so this file builds on host GCC for unit
 *     tests exactly as platform/uds_sha256.c does.
 *   - No endianness assumptions; no packed structs; no pointer casts.
 *
 * SAFETY  : Security-relevant. ASIL-B candidate.
 *           This module has not undergone formal ASIL assessment.
 *           OEM must validate before vehicle deployment.
 * STANDARD: MCUboot image format (bootutil/image.h).
 *           MISRA C:2012 alignment intended.
 *           Deviation log at bottom of uds_mcuboot_image.c.
 * =============================================================================
 */

#ifndef UDS_MCUBOOT_IMAGE_H
#define UDS_MCUBOOT_IMAGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Magic values
 * -------------------------------------------------------------------------- */

/** Current MCUboot image header magic (IMAGE_MAGIC). */
#define UDS_MCUBOOT_IMAGE_MAGIC          (0x96f3b83dUL)

/**
 * Legacy v1 image header magic (IMAGE_MAGIC_V1).
 *
 * REJECTED by uds_mcuboot_parse_header() — see that function's contract.
 * Defined here only so that a caller or a test can name the value it is
 * expected to refuse.
 */
#define UDS_MCUBOOT_IMAGE_MAGIC_V1       (0x96f3b83cUL)

/** image_tlv_info magic for the regular (unprotected) TLV area. */
#define UDS_MCUBOOT_TLV_INFO_MAGIC       (0x6907U)

/** image_tlv_info magic for the PROTECTED TLV area. */
#define UDS_MCUBOOT_TLV_PROT_INFO_MAGIC  (0x6908U)

/* --------------------------------------------------------------------------
 * Size constants
 * -------------------------------------------------------------------------- */

/** Size of the MCUboot image header in bytes (IMAGE_HEADER_SIZE). */
#define UDS_MCUBOOT_HEADER_SIZE          (32U)

/** Size of the image_tlv_info record that opens every TLV area. */
#define UDS_MCUBOOT_TLV_INFO_SIZE        (4U)

/** Size of the image_tlv record that precedes every TLV value. */
#define UDS_MCUBOOT_TLV_HDR_SIZE         (4U)

/** Length of the value carried by a SHA256 TLV (IMAGE_HASH_LEN). */
#define UDS_MCUBOOT_HASH_LEN             (32U)

/* --------------------------------------------------------------------------
 * ih_flags bits (selected)
 * -------------------------------------------------------------------------- */

/** Image body is encrypted with AES-128 (IMAGE_F_ENCRYPTED_AES128). */
#define UDS_MCUBOOT_F_ENCRYPTED_AES128   (0x00000004UL)

/** Image body is encrypted with AES-256 (IMAGE_F_ENCRYPTED_AES256). */
#define UDS_MCUBOOT_F_ENCRYPTED_AES256   (0x00000008UL)

/* --------------------------------------------------------------------------
 * TLV type identifiers
 * -------------------------------------------------------------------------- */

#define UDS_MCUBOOT_TLV_KEYHASH          (0x01U)  /**< Hash of the public key. */
#define UDS_MCUBOOT_TLV_PUBKEY           (0x02U)  /**< Public key.             */
#define UDS_MCUBOOT_TLV_SHA256           (0x10U)  /**< SHA-256 of hdr + body.  */
#define UDS_MCUBOOT_TLV_SHA384           (0x11U)  /**< SHA-384 of hdr + body.  */
#define UDS_MCUBOOT_TLV_RSA2048_PSS      (0x20U)  /**< RSA-2048 PSS signature. */
#define UDS_MCUBOOT_TLV_ECDSA224         (0x21U)  /**< Legacy, unsupported.    */
#define UDS_MCUBOOT_TLV_ECDSA_SIG        (0x22U)  /**< ECDSA P-256 signature.  */
#define UDS_MCUBOOT_TLV_RSA3072_PSS      (0x23U)  /**< RSA-3072 PSS signature. */
#define UDS_MCUBOOT_TLV_ED25519          (0x24U)  /**< Ed25519 signature.      */
#define UDS_MCUBOOT_TLV_ENC_RSA2048      (0x30U)  /**< Key-encryption blob.    */
#define UDS_MCUBOOT_TLV_ENC_KW           (0x31U)  /**< Key-encryption blob.    */
#define UDS_MCUBOOT_TLV_ENC_EC256        (0x32U)  /**< Key-encryption blob.    */
#define UDS_MCUBOOT_TLV_ENC_X25519       (0x33U)  /**< Key-encryption blob.    */
#define UDS_MCUBOOT_TLV_DEPENDENCY       (0x40U)  /**< Image dependency.       */
#define UDS_MCUBOOT_TLV_SEC_CNT          (0x50U)  /**< Security counter.       */
#define UDS_MCUBOOT_TLV_BOOT_RECORD      (0x60U)  /**< CBOR boot record.       */
#define UDS_MCUBOOT_TLV_ANY              (0xffffU) /**< Wildcard (not decoded).*/

/* --------------------------------------------------------------------------
 * Status
 * -------------------------------------------------------------------------- */

/**
 * @brief Outcome of a parse or iteration step.
 *
 * Deliberately NOT uds_status_t: this module sits below the UDS service
 * layer and describes wire-format facts, not diagnostic responses.  The
 * Phase 2 policy maps these onto uds_image_verdict_t (typically
 * UDS_IMAGE_REJECT_FORMAT for every non-OK value).
 */
typedef enum uds_mcuboot_parse_status {
    UDS_MCUBOOT_PARSE_OK = 0,                 /**< Well formed.                */
    UDS_MCUBOOT_PARSE_ERR_NULL_ARG,           /**< A required pointer was NULL.*/
    UDS_MCUBOOT_PARSE_ERR_BUFFER_TOO_SMALL,   /**< Fewer bytes than the fixed
                                                *   structure needs.           */
    UDS_MCUBOOT_PARSE_ERR_BAD_MAGIC,          /**< Magic absent or not the one
                                                *   expected.                  */
    UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH          /**< A declared length is
                                                *   self-inconsistent or runs
                                                *   past the buffer.           */
} uds_mcuboot_parse_status_t;

/* --------------------------------------------------------------------------
 * Decoded image header
 * -------------------------------------------------------------------------- */

/**
 * @brief Decoded MCUboot image header.
 *
 * This is an OUTPUT structure with natural alignment and host byte order.
 * It is never overlaid on a flash buffer — uds_mcuboot_parse_header() fills
 * it field by field from explicitly decoded bytes.  The reserved _pad1 word
 * at offset 28 is not represented; it carries no information.
 */
typedef struct uds_mcuboot_header {
    uint32_t magic;             /**< ih_magic, always UDS_MCUBOOT_IMAGE_MAGIC
                                  *   when the parse returned OK.            */
    uint32_t load_addr;         /**< ih_load_addr.                           */
    uint16_t hdr_size;          /**< ih_hdr_size, >= UDS_MCUBOOT_HEADER_SIZE.*/
    uint16_t protect_tlv_size;  /**< ih_protect_tlv_size, 0 if no protected
                                  *   TLV area is present.                   */
    uint32_t img_size;          /**< ih_img_size — BODY only, header not
                                  *   included.                              */
    uint32_t flags;             /**< ih_flags, see UDS_MCUBOOT_F_*.          */
    uint8_t  ver_major;         /**< ih_ver.iv_major.                        */
    uint8_t  ver_minor;         /**< ih_ver.iv_minor.                        */
    uint16_t ver_revision;      /**< ih_ver.iv_revision.                     */
    uint32_t ver_build_num;     /**< ih_ver.iv_build_num.                    */
} uds_mcuboot_header_t;

/* --------------------------------------------------------------------------
 * TLV iteration
 * -------------------------------------------------------------------------- */

/**
 * @brief Cursor over one TLV area.
 *
 * Initialised by uds_mcuboot_tlv_iter_begin() and advanced by
 * uds_mcuboot_tlv_iter_next().  Treat the members as private; they are
 * published so the struct can live on the caller's stack.
 *
 * INVARIANT maintained by both functions:  cursor <= area_len  at all times.
 * Every bounds test in the implementation is a subtraction against that
 * invariant, never an addition that could overflow.
 */
typedef struct uds_mcuboot_tlv_iter {
    const uint8_t *area;      /**< First byte of the image_tlv_info record.  */
    size_t         area_len;  /**< Total area length, the 4-byte info record
                                *   INCLUDED.                                */
    size_t         cursor;    /**< Offset, relative to area, of the next TLV
                                *   header to read.                          */
} uds_mcuboot_tlv_iter_t;

/**
 * @brief One decoded TLV entry.
 *
 * The value bytes are NOT copied.  They live at
 * `&iter.area[entry.value_offset]` and span `entry.len` bytes, a range the
 * iterator has already proven to lie inside the area.
 */
typedef struct uds_mcuboot_tlv {
    uint16_t type;          /**< it_type, see UDS_MCUBOOT_TLV_*.             */
    uint16_t len;           /**< it_len — length of the VALUE only.          */
    size_t   value_offset;  /**< Offset of the value within `area`.          */
} uds_mcuboot_tlv_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/**
 * @brief Decode a 32-byte MCUboot image header.
 *
 * Every multi-byte field is assembled from individual bytes, so `buf` needs
 * no particular alignment.  `*out` is only meaningful when the function
 * returns UDS_MCUBOOT_PARSE_OK; on any error it is left untouched, so a
 * caller that ignores the return value cannot mistake stale or partially
 * decoded fields for a validated header.
 *
 * V1 MAGIC IS REJECTED.  An image whose ih_magic is
 * UDS_MCUBOOT_IMAGE_MAGIC_V1 (the pre-1.0 MCUboot format, whose header
 * layout and TLV conventions differ) returns
 * UDS_MCUBOOT_PARSE_ERR_BAD_MAGIC exactly as random bytes would.  This
 * repo's Phase 2 reference policy targets current MCUboot only, and
 * silently accepting a legacy header whose fields this decoder would then
 * misread is precisely the fail-open behaviour issue #232 exists to remove.
 *
 * @param[in]  buf      Bytes read from the start of the image.
 * @param[in]  buf_len  Number of bytes available at buf.  May exceed
 *                      UDS_MCUBOOT_HEADER_SIZE; the excess is ignored.
 * @param[out] out      Receives the decoded header on success.
 *
 * @return UDS_MCUBOOT_PARSE_OK on success.
 * @return UDS_MCUBOOT_PARSE_ERR_NULL_ARG if buf or out is NULL.
 * @return UDS_MCUBOOT_PARSE_ERR_BUFFER_TOO_SMALL if buf_len <
 *         UDS_MCUBOOT_HEADER_SIZE.
 * @return UDS_MCUBOOT_PARSE_ERR_BAD_MAGIC if ih_magic is not
 *         UDS_MCUBOOT_IMAGE_MAGIC (v1 included).
 * @return UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH if ih_hdr_size is smaller than
 *         UDS_MCUBOOT_HEADER_SIZE — a header that claims to be shorter than
 *         the fixed structure it just occupied is structurally impossible,
 *         and Phase 2 uses ih_hdr_size to locate the image body.  A LARGER
 *         ih_hdr_size is legitimate (MCUboot pads the header out to the
 *         target's write-block or vector-table alignment) and is accepted.
 */
uds_mcuboot_parse_status_t uds_mcuboot_parse_header(const uint8_t        *buf,
                                                    size_t                buf_len,
                                                    uds_mcuboot_header_t *out);

/**
 * @brief Start iterating a TLV area.
 *
 * PRECONDITION — `area_len` MUST be the area's own declared length.  The
 * caller passes the exact byte range of one TLV area: `area` points at its
 * 4-byte image_tlv_info record and `area_len` equals the it_tlv_tot value
 * inside that record.  Passing the whole flash slot, or the concatenation
 * of the protected and regular areas, is a caller error and is rejected
 * with UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH rather than silently iterating off
 * the end of one area into the next.
 *
 * Where does the caller get area_len from before it has parsed the record?
 * For the protected area, from ih_protect_tlv_size in the image header.
 * For the regular area, by reading the 4-byte info record first and using
 * its it_tlv_tot.  Cross-checking the two is the point: a mismatch means
 * the trailer has been tampered with or truncated.
 *
 * @param[out] it                   Iterator to initialise.  Only meaningful
 *                                  when the function returns OK; left
 *                                  untouched on error.
 * @param[in]  area                 First byte of the image_tlv_info record.
 * @param[in]  area_len             Declared total size of the area, the
 *                                  4-byte record included.
 * @param[in]  expected_info_magic  UDS_MCUBOOT_TLV_INFO_MAGIC for the
 *                                  regular area, or
 *                                  UDS_MCUBOOT_TLV_PROT_INFO_MAGIC for the
 *                                  protected one.  The caller states which
 *                                  area it believes it is looking at; a
 *                                  protected area presented as a regular one
 *                                  is an error, not a detail to paper over.
 *
 * @return UDS_MCUBOOT_PARSE_OK on success; the iterator is positioned just
 *         past the info record.
 * @return UDS_MCUBOOT_PARSE_ERR_NULL_ARG if it or area is NULL.
 * @return UDS_MCUBOOT_PARSE_ERR_BUFFER_TOO_SMALL if area_len <
 *         UDS_MCUBOOT_TLV_INFO_SIZE.
 * @return UDS_MCUBOOT_PARSE_ERR_BAD_MAGIC if it_magic != expected_info_magic.
 * @return UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH if it_tlv_tot is below
 *         UDS_MCUBOOT_TLV_INFO_SIZE (it cannot be smaller than its own
 *         record) or does not equal area_len.
 */
uds_mcuboot_parse_status_t uds_mcuboot_tlv_iter_begin(uds_mcuboot_tlv_iter_t *it,
                                                      const uint8_t          *area,
                                                      size_t                  area_len,
                                                      uint16_t                expected_info_magic);

/**
 * @brief Fetch the next TLV entry.
 *
 * CLEAN END AND CORRUPTION ARE DISTINCT OUTCOMES, and the caller must be
 * able to tell them apart — a policy that treated a truncated trailer as
 * "no more TLVs" would accept an image whose signature TLV had simply been
 * chopped off.  Hence:
 *
 *   return true,  *status == OK           -> *out holds a valid entry
 *   return false, *status == OK           -> clean end of area, nothing more
 *   return false, *status != OK           -> the area is MALFORMED; the
 *                                            caller must fail closed and
 *                                            must not iterate further
 *
 * On any false return `*out` is left untouched, and no out-of-bounds offset
 * or pointer is ever computed, let alone dereferenced.
 *
 * @param[in,out] it      Iterator from uds_mcuboot_tlv_iter_begin().
 * @param[out]    out     Receives the entry when the function returns true.
 * @param[out]    status  Receives the outcome.  Always written when
 *                        non-NULL, including on the true path.
 *
 * @return true if an entry was decoded; false on clean end or on error.
 */
bool uds_mcuboot_tlv_iter_next(uds_mcuboot_tlv_iter_t     *it,
                               uds_mcuboot_tlv_t          *out,
                               uds_mcuboot_parse_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* UDS_MCUBOOT_IMAGE_H */
