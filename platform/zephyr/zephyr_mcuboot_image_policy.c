// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/zephyr_mcuboot_image_policy.c
 *
 * PURPOSE: Zephyr/MCUboot reference image policy implementation.
 *          See zephyr_mcuboot_image_policy.h for the full design rationale
 *          and the #277/#232 scope decision (digest-only; MCUboot itself
 *          remains the sole signature verifier).
 *
 * DIGEST COVERAGE:
 *   MCUboot's own SHA256 TLV (which its bootloader recomputes and checks
 *   before trusting a signature) covers exactly [0, ih_hdr_size + ih_img_size)
 *   of the image — the header (including any padding beyond the fixed
 *   32-byte record MCUboot's imgtool may add to reach a target's
 *   write-block alignment) plus the body. The TLV trailer itself is NOT
 *   part of what it hashes. This module streams the SAME range so the two
 *   digests are comparable.
 *
 *   [PROTECTED TLV] ih_protect_tlv_size != 0 means the image carries a
 *   protected TLV area (dependencies, a hardware security counter) that
 *   MCUboot folds into its own hash ahead of the trailer. This repo's
 *   documented `west sign` invocation (examples/safeboot_ecu/README.md)
 *   does not produce one. Rather than silently mis-hash such an image, an
 *   image that declares one is rejected as REJECT_FORMAT — fail closed, not
 *   fail open, on a configuration this policy was not built to handle.
 *
 * STATE MACHINE (one transfer at a time — REQ-DL-SEQ already enforces this
 * at the service layer, so no re-entrancy guard is needed here):
 *
 *   begin_cb   -> reset context, start streaming
 *   update_cb  -> for image_offset < 32:  buffer into s_hdr_buf
 *                 once 32 bytes are in:   parse header, derive body_end_offset
 *                 for image_offset < body_end_offset: feed uds_sha256_update()
 *                 for image_offset >= body_end_offset: skip (TLV trailer)
 *   finalise_cb -> read the TLV trailer back from flash, find the SHA256
 *                  TLV, compare against uds_sha256_final()
 *   commit_cb   -> boot_request_upgrade(BOOT_UPGRADE_TEST)
 *   abort_cb    -> reset context
 *
 * DESIGN CONSTRAINTS: no dynamic allocation; update_cb is O(length) and
 * allocation-free (uds_image_policy.h's constraint — it runs inside P2).
 *
 * SAFETY  : Security-relevant. ASIL-B candidate.
 *           This module has not undergone formal ASIL assessment.
 *           OEM must validate before vehicle deployment.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "zephyr_mcuboot_image_policy.h"
#include "uds_image_policy.h"
#include "uds_sha256.h"
#include "uds_mcuboot_image.h"
#include "uds_types.h"

#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <zephyr/dfu/mcuboot.h>

LOG_MODULE_REGISTER(zephyr_mcuboot_image_policy, LOG_LEVEL_INF);

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#ifndef FIXED_PARTITION_ID
#  error "FIXED_PARTITION_ID not available — ensure CONFIG_FLASH_MAP=y in prj.conf"
#endif

/** @brief ID of the MCUboot secondary slot (image_1) — same slot the flash
 *         ops table in zephyr_flash_ops.c writes to. */
#define ZEPHYR_IMGPOL_SECONDARY_SLOT  FIXED_PARTITION_ID(image_1)

/**
 * @brief Maximum TLV trailer size this policy will read into RAM.
 *
 * The reference `west sign` invocation (examples/safeboot_ecu/README.md)
 * produces a trailer of: 4-byte tlv_info + a SHA256 TLV (4 + 32 bytes) + an
 * RSA-2048-PSS signature TLV (4 + 256 bytes) = 300 bytes. 512 bytes leaves
 * comfortable headroom without needing dynamic allocation. A trailer larger
 * than this is rejected as REJECT_FORMAT rather than silently truncated.
 */
#define ZEPHYR_IMGPOL_MAX_TLV_AREA_LEN  (512U)

/* --------------------------------------------------------------------------
 * Policy state — one transfer at a time (REQ-DL-SEQ enforced by the caller)
 * -------------------------------------------------------------------------- */

typedef struct {
    bool               active;            /**< A download is in progress.       */
    bool               header_parsed;     /**< s_hdr_buf has been decoded.      */
    bool               digest_ready;      /**< body_end_offset reached; digest
                                            *   in s_digest is final.            */
    bool               hard_fail;         /**< A prior update_cb call already
                                            *   rejected this transfer; every
                                            *   further call is a no-op refusal
                                            *   (defensive — the caller aborts
                                            *   on the first non-OK already).   */

    uds_sha256_ctx_t   sha_ctx;
    uint8_t            hdr_buf[UDS_MCUBOOT_HEADER_SIZE];
    uint16_t           hdr_buf_fill;

    uint32_t           target_address;    /**< Evidence base for finalise_cb.   */
    uint32_t           total_size_bytes;  /**< Declared image size (0x34).      */
    uint32_t           body_end_offset;   /**< hdr_size + img_size.             */

    uint8_t            digest[UDS_SHA256_DIGEST_LEN];
} zephyr_imgpol_state_t;

static zephyr_imgpol_state_t s_st;

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static void s_reset(void)
{
    (void)memset(&s_st, 0, sizeof(s_st));
}

/**
 * @brief Feed [data, data+len) into the running digest, honouring
 *        body_end_offset — bytes at or past it belong to the TLV trailer
 *        and must not be hashed.
 *
 * @param[in] chunk_offset  Absolute image offset of data[0].
 */
static void s_hash_within_body(uint32_t chunk_offset, const uint8_t *data, uint32_t len)
{
    uint32_t chunk_end = chunk_offset + len;
    uint32_t hash_end   = (chunk_end < s_st.body_end_offset) ? chunk_end : s_st.body_end_offset;

    if (hash_end > chunk_offset) {
        uds_sha256_update(&s_st.sha_ctx, data, (size_t)(hash_end - chunk_offset));
    }

    /* Finalise exactly once. Bytes past body_end_offset (the TLV trailer)
     * still arrive here on later calls — chunk_end >= body_end_offset would
     * otherwise be true on every one of them, and uds_sha256_final() is not
     * idempotent (uds_sha256.h: a second call does not reproduce the same
     * digest — the context is scrubbed by the first). */
    if (!s_st.digest_ready && (chunk_end >= s_st.body_end_offset)) {
        uds_sha256_final(&s_st.sha_ctx, s_st.digest);
        s_st.digest_ready = true;
    }
}

/* --------------------------------------------------------------------------
 * uds_image_policy_t callbacks
 * -------------------------------------------------------------------------- */

static uds_status_t s_begin_cb(const uds_image_transfer_info_t *info)
{
    /* Cheapest possible sanity check: an image smaller than a bare header
     * plus a minimal SHA256-only trailer cannot be a real MCUboot image.
     * Refused here (NRC 0x70) rather than left to fail later at 0x37, so a
     * malformed RequestDownload never gets as far as erasing the slot. */
    uint32_t min_plausible =
        (uint32_t)UDS_MCUBOOT_HEADER_SIZE +
        (uint32_t)UDS_MCUBOOT_TLV_INFO_SIZE +
        (uint32_t)UDS_MCUBOOT_TLV_HDR_SIZE +
        (uint32_t)UDS_MCUBOOT_HASH_LEN;

    if ((info == NULL) || (info->total_size_bytes < min_plausible)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    s_reset();
    uds_sha256_init(&s_st.sha_ctx);
    s_st.active           = true;
    s_st.target_address   = info->target_address;
    s_st.total_size_bytes = info->total_size_bytes;

    return UDS_STATUS_OK;
}

static uds_status_t s_update_cb(uint32_t image_offset, const uint8_t *data, uint32_t length)
{
    uint32_t consumed = 0U;

    if (!s_st.active || s_st.hard_fail) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }
    if ((data == NULL) && (length != 0U)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Phase A: still filling the fixed 32-byte header record. */
    if (!s_st.header_parsed) {
        uint32_t need = (uint32_t)UDS_MCUBOOT_HEADER_SIZE - (uint32_t)s_st.hdr_buf_fill;
        uint32_t take = (length < need) ? length : need;

        if (take > 0U) {
            (void)memcpy(&s_st.hdr_buf[s_st.hdr_buf_fill], data, (size_t)take);
            uds_sha256_update(&s_st.sha_ctx, data, (size_t)take);
            s_st.hdr_buf_fill += (uint16_t)take;
            consumed          += take;
        }

        if (s_st.hdr_buf_fill < (uint16_t)UDS_MCUBOOT_HEADER_SIZE) {
            /* Not enough bytes yet across all calls so far — wait for more. */
            return UDS_STATUS_OK;
        }

        {
            uds_mcuboot_header_t       hdr;
            uds_mcuboot_parse_status_t pstatus =
                uds_mcuboot_parse_header(s_st.hdr_buf, sizeof(s_st.hdr_buf), &hdr);

            if (pstatus != UDS_MCUBOOT_PARSE_OK) {
                LOG_ERR("[DFU] Image header parse failed: %d", (int)pstatus);
                s_st.hard_fail = true;
                return UDS_STATUS_ERR_GENERIC;
            }
            if (hdr.protect_tlv_size != 0U) {
                /* See file header comment — protected TLV area unsupported. */
                LOG_ERR("[DFU] Image carries a protected TLV area "
                        "(size %u) — this policy does not support it.",
                        (unsigned)hdr.protect_tlv_size);
                s_st.hard_fail = true;
                return UDS_STATUS_ERR_GENERIC;
            }

            s_st.body_end_offset = hdr.hdr_size + hdr.img_size;
            if ((s_st.body_end_offset < hdr.hdr_size) ||               /* overflow */
                (s_st.body_end_offset >= s_st.total_size_bytes)) {
                /* Header claims a body that doesn't leave room for any
                 * trailer at all within the size declared at 0x34. */
                LOG_ERR("[DFU] Image header/body size inconsistent with "
                        "declared transfer size.");
                s_st.hard_fail = true;
                return UDS_STATUS_ERR_GENERIC;
            }

            s_st.header_parsed = true;
        }
    }

    /* Phase B: stream the remainder of this chunk (if any) as body/TLV. */
    if (consumed < length) {
        uint32_t remaining_offset = image_offset + consumed;

        s_hash_within_body(remaining_offset, &data[consumed], length - consumed);
    }

    return UDS_STATUS_OK;
}

static uds_status_t s_finalise_cb(const uds_image_evidence_t *evidence,
                                   uds_image_verdict_t        *out_verdict)
{
    const struct flash_area *fa = NULL;
    uint8_t                  tlv_buf[ZEPHYR_IMGPOL_MAX_TLV_AREA_LEN];
    uint32_t                 tlv_area_len;
    int                       rc;
    uds_mcuboot_tlv_iter_t    it;
    uds_mcuboot_tlv_t         entry;
    uds_mcuboot_parse_status_t pstatus;
    bool                      found_sha256 = false;

    if ((evidence == NULL) || (out_verdict == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    *out_verdict = UDS_IMAGE_REJECT_FORMAT; /* fail-closed default */

    if (!s_st.active || !s_st.header_parsed || !s_st.digest_ready) {
        LOG_ERR("[DFU] finalise_cb reached without a completed digest.");
        return UDS_STATUS_OK; /* verdict already REJECT_FORMAT */
    }

    tlv_area_len = evidence->total_size_bytes - s_st.body_end_offset;
    if (tlv_area_len > (uint32_t)sizeof(tlv_buf)) {
        LOG_ERR("[DFU] TLV trailer (%u bytes) exceeds the %u-byte read "
                "buffer.", (unsigned)tlv_area_len, (unsigned)sizeof(tlv_buf));
        return UDS_STATUS_OK; /* verdict already REJECT_FORMAT */
    }

    rc = flash_area_open(ZEPHYR_IMGPOL_SECONDARY_SLOT, &fa);
    if (rc != 0) {
        LOG_ERR("[DFU] flash_area_open(image_1) failed: %d", rc);
        return UDS_STATUS_ERR_PLATFORM;
    }

    rc = flash_area_read(fa,
                          (off_t)(s_st.body_end_offset),
                          tlv_buf,
                          (size_t)tlv_area_len);
    flash_area_close(fa);

    if (rc != 0) {
        LOG_ERR("[DFU] flash_area_read(TLV trailer) failed: %d", rc);
        return UDS_STATUS_ERR_PLATFORM;
    }

    pstatus = uds_mcuboot_tlv_iter_begin(&it, tlv_buf, (size_t)tlv_area_len,
                                          UDS_MCUBOOT_TLV_INFO_MAGIC);
    if (pstatus != UDS_MCUBOOT_PARSE_OK) {
        LOG_ERR("[DFU] TLV trailer malformed at info record: %d", (int)pstatus);
        return UDS_STATUS_OK; /* verdict already REJECT_FORMAT */
    }

    while (uds_mcuboot_tlv_iter_next(&it, &entry, &pstatus)) {
        if ((entry.type == UDS_MCUBOOT_TLV_SHA256) &&
            (entry.len  == (uint16_t)UDS_MCUBOOT_HASH_LEN)) {
            found_sha256 = true;
            if (memcmp(&tlv_buf[entry.value_offset], s_st.digest,
                       (size_t)UDS_MCUBOOT_HASH_LEN) == 0) {
                *out_verdict = UDS_IMAGE_ACCEPT;
            } else {
                LOG_ERR("[DFU] SHA-256 digest mismatch — image's own TLV "
                        "does not match the streamed digest.");
                *out_verdict = UDS_IMAGE_REJECT_DIGEST;
            }
            break;
        }
    }

    if (pstatus != UDS_MCUBOOT_PARSE_OK) {
        LOG_ERR("[DFU] TLV trailer malformed mid-iteration: %d", (int)pstatus);
        *out_verdict = UDS_IMAGE_REJECT_FORMAT;
    } else if (!found_sha256) {
        LOG_ERR("[DFU] No SHA256 TLV found in trailer.");
        *out_verdict = UDS_IMAGE_REJECT_FORMAT;
    }

    if (*out_verdict == UDS_IMAGE_ACCEPT) {
        LOG_INF("[DFU] Image digest verified OK (%u bytes header+body).",
                (unsigned)s_st.body_end_offset);
    }

    return UDS_STATUS_OK;
}

static uds_status_t s_commit_cb(void)
{
    int rc = boot_request_upgrade(BOOT_UPGRADE_TEST);

    if (rc != 0) {
        LOG_ERR("[DFU] boot_request_upgrade() failed: %d", rc);
        return UDS_STATUS_ERR_PLATFORM;
    }

    LOG_INF("[DFU] Swap armed (BOOT_UPGRADE_TEST) — new image runs once on "
            "next reset and must confirm itself, or MCUboot reverts it.");
    s_reset();
    return UDS_STATUS_OK;
}

static void s_abort_cb(void)
{
    s_reset();
}

/* --------------------------------------------------------------------------
 * Policy table + init
 * -------------------------------------------------------------------------- */

static const uds_image_policy_t s_policy = {
    .begin_cb     = s_begin_cb,
    .update_cb    = s_update_cb,
    .finalise_cb  = s_finalise_cb,
    .commit_cb    = s_commit_cb,
    .abort_cb     = s_abort_cb,
    .policy_flags = 0U,   /* No signature check at this layer — see file header. */
};

uds_status_t zephyr_mcuboot_image_policy_init(void)
{
    s_reset();
    return uds_image_policy_register(&s_policy);
}
