// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/uds_mcuboot_image.c
 *
 * PURPOSE: Pure-logic parser for the MCUboot image header and TLV trailer.
 *
 * PHASE 2 — DFU firmware authenticity, hardware-independent half [#232]
 *
 * See uds_mcuboot_image.h for the wire format and the full API contract.
 *
 * IMPLEMENTATION NOTES:
 *
 *   No struct overlay, ever
 *   -----------------------
 *   The obvious implementation of this file is
 *
 *       const struct image_header *h = (const struct image_header *)buf;
 *
 *   and it is wrong three times over.  (a) `buf` comes from a flash read or
 *   from the 0x36 reassembly buffer and carries no alignment guarantee, so
 *   the dereference is undefined behaviour on any target that traps
 *   unaligned word access.  (b) The result depends on host endianness and
 *   on the compiler honouring the packing attribute, neither of which is
 *   part of the wire format.  (c) It is a MISRA C:2012 Rule 11.3 violation
 *   (cast between pointers to objects of incompatible type), which this
 *   repo's core/ carries zero of.  Every field below is therefore assembled
 *   from individual bytes by shift-and-or, which compiles to the same code
 *   as the cast on a little-endian target that permits unaligned access,
 *   and to correct code everywhere else.
 *
 *   Bounds arithmetic that cannot overflow
 *   --------------------------------------
 *   The hostile input here is a 16-bit length field — it_tlv_tot or it_len
 *   — chosen by whoever produced the image.  The naive check
 *
 *       if ((cursor + 4U + len) > area_len) { ...reject... }
 *
 *   can wrap when cursor is already near SIZE_MAX, and then accepts.  Every
 *   bounds test in this file is instead written as a SUBTRACTION from a
 *   quantity already proven to be an upper bound:
 *
 *       remaining = area_len - cursor;        -- safe: cursor <= area_len
 *       if (remaining < UDS_MCUBOOT_TLV_HDR_SIZE) { ...reject... }
 *       remaining -= UDS_MCUBOOT_TLV_HDR_SIZE;
 *       if ((size_t)len > remaining)          { ...reject... }
 *
 *   The invariant cursor <= area_len is established by iter_begin() and
 *   re-established by every iter_next() before it returns, so the first
 *   subtraction can never underflow.  Nothing in the failure paths computes
 *   `area + something_too_big`, so there is no out-of-bounds pointer even
 *   in the unevaluated sense the standard objects to — this holds under
 *   ASan/UBSan, which build_tests.sh --sanitize runs over these tests.
 *
 *   Why the iterator does not copy values
 *   -------------------------------------
 *   A signature TLV can be a few hundred bytes; the no-dynamic-allocation
 *   rule means a copying iterator would need a worst-case static buffer for
 *   the largest TLV any image might carry.  Returning a validated OFFSET
 *   instead keeps the module allocation-free and zero-copy, and the caller
 *   already holds the buffer.
 *
 * MISRA C:2012 DEVIATION LOG:
 *   [DEV-MCUB-01] Rule 15.5 (single point of exit) — the three public
 *     functions use guard clauses for argument and bounds validation.
 *     Deviation justified: a single-exit rewrite nests every subsequent
 *     check one level deeper and obscures exactly the bounds reasoning that
 *     most needs to be reviewable.  Same pattern as the guard clauses in
 *     platform/uds_image_policy.c.
 *   [DEV-MCUB-02] Rule 10.3/10.4 — byte-assembly expressions cast each
 *     shifted operand to the target width (uint32_t / uint16_t) before the
 *     OR, so the essential type of the result matches the field being
 *     decoded rather than the `int` the integer promotions would otherwise
 *     produce.
 *   [DEV-MCUB-03] Rule 12.2 — every shift count is a literal 8, 16 or 24
 *     applied to a uint32_t operand, or 8 applied to a uint16_t operand
 *     already promoted to uint32_t.  No count can reach the operand width.
 *   [DEV-MCUB-04] Directive 4.6 — `bool` is used as the return type of
 *     uds_mcuboot_tlv_iter_next().  <stdbool.h> is already used throughout
 *     platform/ and core/ (uds_image_policy.h, uds_flash_ops.h,
 *     uds_transfer_ctx.h); introducing a module-local integer convention
 *     here would be the inconsistency.
 *
 * SAFETY  : Security-relevant. ASIL-B candidate. See uds_mcuboot_image.h.
 * STANDARD: MCUboot image format. MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "uds_mcuboot_image.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Field offsets within the 32-byte image_header
 * -------------------------------------------------------------------------- */

#define HDR_OFF_MAGIC             (0U)
#define HDR_OFF_LOAD_ADDR         (4U)
#define HDR_OFF_HDR_SIZE          (8U)
#define HDR_OFF_PROTECT_TLV_SIZE  (10U)
#define HDR_OFF_IMG_SIZE          (12U)
#define HDR_OFF_FLAGS             (16U)
#define HDR_OFF_VER_MAJOR         (20U)
#define HDR_OFF_VER_MINOR         (21U)
#define HDR_OFF_VER_REVISION      (22U)
#define HDR_OFF_VER_BUILD_NUM     (24U)
/* Offset 28 is _pad1, reserved and deliberately not decoded. */

/* --------------------------------------------------------------------------
 * Field offsets within the 4-byte image_tlv_info and image_tlv records
 * -------------------------------------------------------------------------- */

#define TLV_INFO_OFF_MAGIC        (0U)
#define TLV_INFO_OFF_TOT          (2U)

#define TLV_OFF_TYPE              (0U)
#define TLV_OFF_LEN               (2U)

/* --------------------------------------------------------------------------
 * Little-endian byte decoders
 * -------------------------------------------------------------------------- */

/**
 * @brief Assemble a little-endian 16-bit field from two bytes.
 *
 * Explicit assembly — never a pointer cast — so the result is independent
 * of host endianness and of the alignment of `b`.
 *
 * @param[in] b  Pointer to the first (least significant) byte.
 * @return  The decoded value.
 */
static uint16_t mcub_load_le16(const uint8_t *b)
{
    /* [DEV-MCUB-02] [DEV-MCUB-03] */
    return (uint16_t)(((uint16_t)b[0U]) |
                      ((uint16_t)((uint16_t)b[1U] << 8U)));
}

/**
 * @brief Assemble a little-endian 32-bit field from four bytes.
 *
 * @param[in] b  Pointer to the first (least significant) byte.
 * @return  The decoded value.
 */
static uint32_t mcub_load_le32(const uint8_t *b)
{
    /* [DEV-MCUB-02] [DEV-MCUB-03] */
    return (uint32_t)(((uint32_t)b[0U])        |
                      ((uint32_t)b[1U] <<  8U) |
                      ((uint32_t)b[2U] << 16U) |
                      ((uint32_t)b[3U] << 24U));
}

/* --------------------------------------------------------------------------
 * Public: image header
 * -------------------------------------------------------------------------- */

uds_mcuboot_parse_status_t uds_mcuboot_parse_header(const uint8_t        *buf,
                                                    size_t                buf_len,
                                                    uds_mcuboot_header_t *out)
{
    uint32_t magic;
    uint16_t hdr_size;

    /* [DEV-MCUB-01] guard clauses. */
    if ((buf == NULL) || (out == NULL)) {
        return UDS_MCUBOOT_PARSE_ERR_NULL_ARG;
    }
    if (buf_len < (size_t)UDS_MCUBOOT_HEADER_SIZE) {
        return UDS_MCUBOOT_PARSE_ERR_BUFFER_TOO_SMALL;
    }

    /* Magic first: nothing else in the buffer means anything until it
     * matches, and UDS_MCUBOOT_IMAGE_MAGIC_V1 is not a match.  See the
     * header for why the legacy format is refused rather than adapted to. */
    magic = mcub_load_le32(&buf[HDR_OFF_MAGIC]);
    if (magic != (uint32_t)UDS_MCUBOOT_IMAGE_MAGIC) {
        return UDS_MCUBOOT_PARSE_ERR_BAD_MAGIC;
    }

    /* A header cannot declare itself shorter than the fixed 32-byte record
     * that was just read out of it.  Larger is legitimate: MCUboot pads the
     * header to the target's alignment requirement. */
    hdr_size = mcub_load_le16(&buf[HDR_OFF_HDR_SIZE]);
    if ((size_t)hdr_size < (size_t)UDS_MCUBOOT_HEADER_SIZE) {
        return UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH;
    }

    /* Only now is *out written: a caller that ignores the return value
     * cannot be handed half-decoded fields from a rejected header. */
    out->magic            = magic;
    out->load_addr        = mcub_load_le32(&buf[HDR_OFF_LOAD_ADDR]);
    out->hdr_size         = hdr_size;
    out->protect_tlv_size = mcub_load_le16(&buf[HDR_OFF_PROTECT_TLV_SIZE]);
    out->img_size         = mcub_load_le32(&buf[HDR_OFF_IMG_SIZE]);
    out->flags            = mcub_load_le32(&buf[HDR_OFF_FLAGS]);
    out->ver_major        = buf[HDR_OFF_VER_MAJOR];
    out->ver_minor        = buf[HDR_OFF_VER_MINOR];
    out->ver_revision     = mcub_load_le16(&buf[HDR_OFF_VER_REVISION]);
    out->ver_build_num    = mcub_load_le32(&buf[HDR_OFF_VER_BUILD_NUM]);

    return UDS_MCUBOOT_PARSE_OK;
}

/* --------------------------------------------------------------------------
 * Public: TLV area iteration
 * -------------------------------------------------------------------------- */

uds_mcuboot_parse_status_t uds_mcuboot_tlv_iter_begin(uds_mcuboot_tlv_iter_t *it,
                                                      const uint8_t          *area,
                                                      size_t                  area_len,
                                                      uint16_t                expected_info_magic)
{
    uint16_t info_magic;
    uint16_t tlv_tot;

    /* [DEV-MCUB-01] guard clauses. */
    if ((it == NULL) || (area == NULL)) {
        return UDS_MCUBOOT_PARSE_ERR_NULL_ARG;
    }
    if (area_len < (size_t)UDS_MCUBOOT_TLV_INFO_SIZE) {
        return UDS_MCUBOOT_PARSE_ERR_BUFFER_TOO_SMALL;
    }

    info_magic = mcub_load_le16(&area[TLV_INFO_OFF_MAGIC]);
    if (info_magic != expected_info_magic) {
        /* Includes the protected-vs-regular mix-up, which is a real
         * tampering signal and must not be normalised away. */
        return UDS_MCUBOOT_PARSE_ERR_BAD_MAGIC;
    }

    tlv_tot = mcub_load_le16(&area[TLV_INFO_OFF_TOT]);
    if ((size_t)tlv_tot < (size_t)UDS_MCUBOOT_TLV_INFO_SIZE) {
        /* An area cannot be smaller than the record that declares it. */
        return UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH;
    }
    if ((size_t)tlv_tot != area_len) {
        /* Documented precondition: the caller passes the area's own
         * declared length.  A mismatch means either a caller error or a
         * tampered trailer; both fail closed here rather than being
         * reconciled by trusting one of the two numbers. */
        return UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH;
    }

    it->area     = area;
    it->area_len = area_len;
    /* Establishes the invariant cursor <= area_len: tlv_tot == area_len and
     * tlv_tot >= UDS_MCUBOOT_TLV_INFO_SIZE were both just checked. */
    it->cursor   = (size_t)UDS_MCUBOOT_TLV_INFO_SIZE;

    return UDS_MCUBOOT_PARSE_OK;
}

bool uds_mcuboot_tlv_iter_next(uds_mcuboot_tlv_iter_t     *it,
                               uds_mcuboot_tlv_t          *out,
                               uds_mcuboot_parse_status_t *status)
{
    size_t   remaining;
    size_t   value_offset;
    uint16_t type;
    uint16_t len;

    /* [DEV-MCUB-01] guard clauses.  status may itself be the NULL argument,
     * so it is written only once it is known to be usable. */
    if (status == NULL) {
        return false;
    }
    if ((it == NULL) || (out == NULL) || (it->area == NULL)) {
        *status = UDS_MCUBOOT_PARSE_ERR_NULL_ARG;
        return false;
    }
    if (it->cursor > it->area_len) {
        /* Cannot happen while the invariant holds; treated as corruption
         * rather than trusted, so that a future edit which breaks the
         * invariant fails closed instead of reading out of bounds. */
        *status = UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH;
        return false;
    }

    remaining = (size_t)(it->area_len - it->cursor);

    if (remaining == (size_t)0U) {
        /* CLEAN end of area: the previous entry ended exactly on the
         * declared boundary.  Not an error. */
        *status = UDS_MCUBOOT_PARSE_OK;
        return false;
    }

    if (remaining < (size_t)UDS_MCUBOOT_TLV_HDR_SIZE) {
        /* Trailing bytes too few to hold a TLV header: the area is
         * truncated or padded with garbage.  MALFORMED, not a clean end —
         * the distinction the whole API exists to preserve. */
        *status = UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH;
        return false;
    }

    type = mcub_load_le16(&it->area[it->cursor + (size_t)TLV_OFF_TYPE]);
    len  = mcub_load_le16(&it->area[it->cursor + (size_t)TLV_OFF_LEN]);

    /* Bounds check by subtraction from an already-proven upper bound; see
     * the overflow discussion in the file banner. */
    remaining -= (size_t)UDS_MCUBOOT_TLV_HDR_SIZE;
    if ((size_t)len > remaining) {
        /* The entry claims a value longer than the area has left.  No
         * pointer or offset past the end is computed on this path. */
        *status = UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH;
        return false;
    }

    value_offset = it->cursor + (size_t)UDS_MCUBOOT_TLV_HDR_SIZE;

    out->type         = type;
    out->len          = len;
    out->value_offset = value_offset;

    /* Re-establish cursor <= area_len: value_offset + len <=
     * value_offset + remaining == area_len, by the check above. */
    it->cursor = value_offset + (size_t)len;

    *status = UDS_MCUBOOT_PARSE_OK;
    return true;
}
