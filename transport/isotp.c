// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: transport/isotp.c
 *
 * PURPOSE: ISO 15765-2 (ISO-TP) transport layer — complete implementation.
 *
 * PHASE-2 FIXES APPLIED:
 *   [P2-TP-01] SF reception and dispatch — fully implemented.
 *   [P2-TP-02] FF reception and FC transmission — fully implemented.
 *   [P2-TP-03] CF reception and reassembly — fully implemented.
 *   [P2-TP-04] FC reception and segmented TX state machine — fully implemented.
 *   [P2-TP-05] As/Bs/Cr timeout enforcement in isotp_tick_1ms.
 *   [P2-TP-06] STmin inter-frame delay for TX consecutive frames.
 *   [P2-TP-07] CAN FD SF and FF encoding added (ISO 15765-2 §9.8):
 *              SF RX/TX: byte 0 = 0x00, byte 1 = SF_DL (1-62) when frame->is_fd.
 *              FF RX/TX: escape sequence (bytes 0-1 = 0x10 0x00, bytes 2-5 = 32-bit
 *              FF_DL) when use_fd and length > UDS_MAX_PAYLOAD_LEN.
 *
 * MULTI-FRAME TX SEQUENCE:
 *   isotp_transmit()            — sends FF, transitions to TX_WAIT_FC
 *   isotp_process_rx_frame()    — on FC CTS: extracts BS/STmin, transitions to TX_SEND_CF
 *   isotp_tx_pump()             — called by isotp_tick_1ms() every 1ms;
 *                                 sends one CF per STmin interval
 *
 * TIMING CONTRACT:
 *   isotp_tick_1ms() MUST be called at 1 ms resolution.
 *   STmin values 0x00–0x7F:   0–127 ms (1 ms resolution).
 *   STmin values 0xF1–0xF9:   100–900 µs (sub-ms; rounded up to 1 ms tick).
 *   STmin values 0x80–0xF0 and 0xFA–0xFF: reserved → treated as 0 ms.
 *
 * SAFETY  : ASIL-B candidate. Full implementation requires formal review.
 * STANDARD: ISO 15765-2:2016.  MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "isotp.h"
#include "uds_types.h"
#include "can_transport.h"

#include <string.h>

/* --------------------------------------------------------------------------
 * Internal helper prototypes
 * -------------------------------------------------------------------------- */

/** Extract ISO-TP frame type nibble from first byte of CAN data. */
static uint8_t isotp_get_frame_type(const uds_can_frame_t *frame);

/** Build and transmit a Flow Control frame. */
static uds_status_t isotp_send_fc(
    isotp_ctx_t *ctx,
    uint8_t      flow_status,
    uint8_t      block_size,
    uint8_t      stmin);

#if ISOTP_TX_PADDING
#if ISOTP_ENABLE_CAN_FD
/**
 * Round min_bytes up to the next valid CAN FD DLC value.
 * Valid FD DLCs above 8: 12, 16, 20, 24, 32, 48, 64.
 */
static uint8_t isotp_fd_round_dlc(uint8_t min_bytes);
#endif /* ISOTP_ENABLE_CAN_FD */

/**
 * REQ-TP-PAD-001: Fill frame bytes [used..total) with ISOTP_TX_PADDING_BYTE.
 * Called immediately before can_transport_transmit() at every TX site.
 */
static void isotp_pad_frame(uint8_t *data, uint8_t used, uint8_t total);
#endif /* ISOTP_TX_PADDING */

/**
 * [P2-TP-06] Decode the STmin field from an FC frame into milliseconds.
 *
 * ISO 15765-2 Table 14:
 *   0x00–0x7F: 0–127 ms (direct value).
 *   0xF1–0xF9: 100–900 µs (sub-millisecond; rounded UP to 1 ms).
 *   All others: reserved — treated as 0 ms (send as fast as possible).
 */
static uint8_t isotp_decode_stmin_ms(uint8_t stmin_raw);

/**
 * [P2-TP-04] [P2-TP-06] Pump the TX consecutive-frame state machine.
 * Called from isotp_tick_1ms() every 1 ms tick.
 * Sends one CF per STmin interval when in TX_SEND_CF state.
 */
static void isotp_tx_pump(isotp_ctx_t *ctx);

/* --------------------------------------------------------------------------
 * Public API implementations
 * -------------------------------------------------------------------------- */

uds_status_t isotp_init(isotp_ctx_t *ctx, const isotp_cfg_t *cfg)
{
    if ((ctx == NULL) || (cfg == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (ctx->initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    if (cfg->can == NULL) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if ((cfg->rx_can_id == 0U) || (cfg->tx_can_id == 0U)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    (void)memset(ctx, 0, sizeof(isotp_ctx_t));

    ctx->rx_can_id        = cfg->rx_can_id;
    ctx->tx_can_id        = cfg->tx_can_id;
    ctx->local_block_size = cfg->block_size;
    ctx->local_stmin_ms   = cfg->stmin_ms;
#if ISOTP_ENABLE_CAN_FD
    ctx->use_fd           = cfg->use_fd;
#endif
    ctx->can              = cfg->can;
    ctx->rx_state         = ISOTP_STATE_IDLE;
    ctx->tx_state         = ISOTP_STATE_IDLE;
    ctx->initialized      = true;

    return UDS_STATUS_OK;
}

uds_status_t isotp_process_rx_frame(
    isotp_ctx_t            *ctx,
    const uds_can_frame_t  *frame,
    isotp_rx_complete_cb    rx_cb,
    void                   *rx_cb_arg)
{
    uint8_t frame_type;

    if ((ctx == NULL) || (frame == NULL) || (rx_cb == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (frame->dlc == (uint8_t)0U) {
        return UDS_STATUS_ERR_TP_FRAME_INVALID;
    }

    frame_type = isotp_get_frame_type(frame);

    switch (frame_type) {

        /* ----------------------------------------------------------------
         * [P2-TP-01] Single Frame (SF) reception
         * [P2-TP-07] CAN FD SF: byte 0 = 0x00, byte 1 = SF_DL (ISO 15765-2 §9.8.1)
         * ---------------------------------------------------------------- */
        case (uint8_t)ISOTP_FRAME_TYPE_SF: {
            uint8_t sf_dl;

#if ISOTP_ENABLE_CAN_FD
            /* CAN FD Single Frame: signalled by frame->is_fd and byte 0 == 0x00. */
            if (frame->is_fd && (frame->data[0] == (uint8_t)0x00U)) {
                sf_dl = frame->data[1];

                if (sf_dl == (uint8_t)0U) {
                    return UDS_STATUS_ERR_TP_FRAME_INVALID;
                }
                if (sf_dl > (uint8_t)ISOTP_FD_SF_MAX_PAYLOAD_LEN) {
                    return UDS_STATUS_ERR_TP_OVERFLOW;
                }
                /* Need PCI (2 bytes) + sf_dl data bytes. */
                if (frame->dlc < (uint8_t)((uint8_t)2U + sf_dl)) {
                    return UDS_STATUS_ERR_TP_FRAME_INVALID;
                }

                ctx->rx_state = ISOTP_STATE_IDLE;
                (void)memcpy(ctx->rx_buf, &frame->data[2], (size_t)sf_dl);
                rx_cb(ctx->rx_buf, (uint32_t)sf_dl, rx_cb_arg);
                return UDS_STATUS_OK;
            }
#endif /* ISOTP_ENABLE_CAN_FD */

            /* Classic CAN Single Frame. */
            sf_dl = (uint8_t)(frame->data[0] & (uint8_t)0x0FU);

            if (sf_dl == (uint8_t)0U) {
                return UDS_STATUS_ERR_TP_FRAME_INVALID;
            }

            if (sf_dl > (uint8_t)(frame->dlc - (uint8_t)1U)) {
                return UDS_STATUS_ERR_TP_FRAME_INVALID;
            }

            /*
             * [MISRA 14.3] Guard against rx_buf overflow using the named
             * protocol constant rather than sizeof().
             *
             * sizeof(ctx->rx_buf) == ISOTP_RX_BUF_LEN which, when cast to
             * uint8_t for a >255-byte buffer, would wrap — the named constant
             * keeps the comparison type-clean.  A Classic CAN SF carries at
             * most 7 data bytes, so any valid SF always fits.
             */
            if (sf_dl > (uint8_t)ISOTP_SF_MAX_PAYLOAD_LEN) {
                return UDS_STATUS_ERR_TP_OVERFLOW;
            }

            /* Abort any in-progress multi-frame RX. */
            ctx->rx_state = ISOTP_STATE_IDLE;

            (void)memcpy(ctx->rx_buf, &frame->data[1], (size_t)sf_dl);
            rx_cb(ctx->rx_buf, (uint32_t)sf_dl, rx_cb_arg);
            return UDS_STATUS_OK;
        }

        /* ----------------------------------------------------------------
         * [P2-TP-02] First Frame (FF) reception
         * [P2-TP-07] CAN FD FF escape sequence for FF_DL > 4095 bytes
         *            (ISO 15765-2 §9.8.2): bytes 0-1 = 0x10 0x00,
         *            bytes 2-5 = 32-bit big-endian FF_DL.
         * ---------------------------------------------------------------- */
        case (uint8_t)ISOTP_FRAME_TYPE_FF: {
            uint32_t ff_dl;
            uint8_t  ff_data_bytes;
            uint8_t  ff_data_offset;

            /* FF_DL: 12 bits from (byte0 & 0x0F) << 8 | byte1. */
            ff_dl = (uint32_t)(((uint32_t)(frame->data[0] & (uint8_t)0x0FU) << 8U)
                               | (uint32_t)frame->data[1]);

            if (ff_dl == (uint32_t)0U) {
#if ISOTP_ENABLE_CAN_FD
                /*
                 * ISO 15765-2 §9.8.2: FF_DL == 0 signals the CAN FD escape
                 * sequence.  Classic CAN never uses this encoding — reject it.
                 */
                if (!frame->is_fd) {
                    return UDS_STATUS_ERR_TP_FRAME_INVALID;
                }
                /* Escape header requires at least 6 bytes: 2 PCI + 4 length. */
                if (frame->dlc < (uint8_t)6U) {
                    return UDS_STATUS_ERR_TP_FRAME_INVALID;
                }
                /* Extract 32-bit FF_DL from bytes 2-5 (big-endian). */
                ff_dl = ((uint32_t)frame->data[2] << 24U)
                      | ((uint32_t)frame->data[3] << 16U)
                      | ((uint32_t)frame->data[4] <<  8U)
                      |  (uint32_t)frame->data[5];

                if (ff_dl == (uint32_t)0U) {
                    return UDS_STATUS_ERR_TP_FRAME_INVALID;
                }
                ff_data_offset = (uint8_t)6U;
#else
                /* FF_DL == 0 is reserved on Classic CAN — always reject. */
                return UDS_STATUS_ERR_TP_FRAME_INVALID;
#endif /* ISOTP_ENABLE_CAN_FD */
            } else {
                /* Standard 12-bit FF_DL (values 1-4095). */
                ff_data_offset = (uint8_t)2U;
            }

            /* Reject if assembled PDU exceeds the static RX buffer. */
            if (ff_dl > (uint32_t)ISOTP_RX_BUF_LEN) {
                (void)isotp_send_fc(ctx,
                                    (uint8_t)ISOTP_FC_STATUS_OVERFLOW,
                                    (uint8_t)0U,
                                    (uint8_t)0U);
                return UDS_STATUS_ERR_TP_OVERFLOW;
            }

            /* Need at least the PCI header + some data. */
            if (frame->dlc <= ff_data_offset) {
                return UDS_STATUS_ERR_TP_FRAME_INVALID;
            }

            ff_data_bytes = (uint8_t)(frame->dlc - ff_data_offset);
            if ((uint32_t)ff_data_bytes > ff_dl) {
                ff_data_bytes = (uint8_t)ff_dl;
            }

            (void)memcpy(ctx->rx_buf, &frame->data[ff_data_offset], (size_t)ff_data_bytes);

            ctx->rx_expected_len = ff_dl;
            ctx->rx_received_len = (uint32_t)ff_data_bytes;
            ctx->rx_expected_sn  = (uint8_t)1U;
            ctx->rx_cr_timer_ms  = (uint32_t)ISOTP_TIMEOUT_CR_MS;
            ctx->rx_state        = ISOTP_STATE_RX_WAIT_CF;

            /* Send FC CTS — best-effort; ignore transmit errors. */
            (void)isotp_send_fc(ctx,
                                (uint8_t)ISOTP_FC_STATUS_CONTINUE_TO_SEND,
                                ctx->local_block_size,
                                ctx->local_stmin_ms);

            return UDS_STATUS_OK;
        }

        /* ----------------------------------------------------------------
         * [P2-TP-03] Consecutive Frame (CF) reception
         * ---------------------------------------------------------------- */
        case (uint8_t)ISOTP_FRAME_TYPE_CF: {
            uint8_t  sn;
            uint32_t remaining;
            uint8_t  cf_data;
            uint32_t copy_len;

            if (ctx->rx_state != ISOTP_STATE_RX_WAIT_CF) {
                return UDS_STATUS_ERR_TP_UNEXPECTED_PDU;
            }

            /* SN = lower nibble of byte 0; wraps 0..15. */
            sn = (uint8_t)(frame->data[0] & (uint8_t)0x0FU);
            if (sn != (uint8_t)(ctx->rx_expected_sn & (uint8_t)0x0FU)) {
                ctx->rx_state = ISOTP_STATE_ERROR;
                return UDS_STATUS_ERR_TP_UNEXPECTED_PDU;
            }

            /* Need at least CF PCI (1 byte) + some data. */
            if (frame->dlc < (uint8_t)2U) {
                ctx->rx_state = ISOTP_STATE_ERROR;
                return UDS_STATUS_ERR_TP_FRAME_INVALID;
            }

            remaining = ctx->rx_expected_len - ctx->rx_received_len;
            cf_data   = (uint8_t)(frame->dlc - (uint8_t)1U);
            copy_len  = ((uint32_t)cf_data < remaining) ? (uint32_t)cf_data : remaining;

            if ((ctx->rx_received_len + copy_len) > (uint32_t)sizeof(ctx->rx_buf)) {
                ctx->rx_state = ISOTP_STATE_ERROR;
                return UDS_STATUS_ERR_TP_OVERFLOW;
            }

            (void)memcpy(&ctx->rx_buf[ctx->rx_received_len],
                         &frame->data[1], (size_t)copy_len);

            ctx->rx_received_len = ctx->rx_received_len + copy_len;
            ctx->rx_expected_sn  = (uint8_t)(ctx->rx_expected_sn + (uint8_t)1U);

            /* Reset Cr timer on each received CF. */
            ctx->rx_cr_timer_ms = (uint32_t)ISOTP_TIMEOUT_CR_MS;

            if (ctx->rx_received_len >= ctx->rx_expected_len) {
                ctx->rx_state = ISOTP_STATE_IDLE;
                rx_cb(ctx->rx_buf, ctx->rx_expected_len, rx_cb_arg);
            }

            return UDS_STATUS_OK;
        }

        /* ----------------------------------------------------------------
         * [P2-TP-04] Flow Control (FC) reception — TX side state machine
         * ---------------------------------------------------------------- */
        case (uint8_t)ISOTP_FRAME_TYPE_FC: {
            uint8_t fs;
            uint8_t bs;
            uint8_t stmin_raw;

            if (ctx->tx_state != ISOTP_STATE_TX_WAIT_FC) {
                /* FC received outside of expected window — ignore silently. */
                return UDS_STATUS_OK;
            }

            fs        = (uint8_t)(frame->data[0] & (uint8_t)0x0FU);
            bs        = frame->data[1];
            stmin_raw = frame->data[2];

            /* Stop Bs timer — FC received in time. */
            ctx->tx_bs_timer_ms = 0U;

            switch (fs) {
                case (uint8_t)ISOTP_FC_STATUS_CONTINUE_TO_SEND:
                    /* [P2-TP-06] Extract and decode STmin. */
                    ctx->tx_block_size    = bs;
                    ctx->tx_stmin_ms      = isotp_decode_stmin_ms(stmin_raw);
                    ctx->tx_blocks_sent   = (uint8_t)0U;

                    /*
                     * [P2-TP-06] Arm STmin timer.
                     * If STmin == 0, send first CF immediately (timer = 0
                     * means fire on the very next tick).
                     */
                    ctx->tx_stmin_timer_ms = (uint32_t)ctx->tx_stmin_ms;
                    ctx->tx_state          = ISOTP_STATE_TX_SEND_CF;
                    break;

                case (uint8_t)ISOTP_FC_STATUS_WAIT:
                    /*
                     * Receiver not ready — restart Bs timer and stay in
                     * TX_WAIT_FC. The next FC will re-enter this handler.
                     */
                    ctx->tx_bs_timer_ms = (uint32_t)ISOTP_TIMEOUT_BS_MS;
                    break;

                case (uint8_t)ISOTP_FC_STATUS_OVERFLOW:
                    ctx->tx_state = ISOTP_STATE_ERROR;
                    return UDS_STATUS_ERR_TP_OVERFLOW;

                default:
                    /* Reserved FS value — treat as abort. */
                    ctx->tx_state = ISOTP_STATE_ERROR;
                    return UDS_STATUS_ERR_TP_FRAME_INVALID;
            }

            return UDS_STATUS_OK;
        }

        default:
            return UDS_STATUS_ERR_TP_FRAME_INVALID;
    }
}

uds_status_t isotp_transmit(
    isotp_ctx_t    *ctx,
    const uint8_t  *data,
    uint32_t        length)
{
    if ((ctx == NULL) || (data == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (length == (uint32_t)0U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

#if ISOTP_ENABLE_CAN_FD
    /* CAN FD allows payloads > 4095; Classic CAN is capped at UDS_MAX_PAYLOAD_LEN. */
    if (!ctx->use_fd && (length > (uint32_t)UDS_MAX_PAYLOAD_LEN)) {
#else
    if (length > (uint32_t)UDS_MAX_PAYLOAD_LEN) {
#endif
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    if (ctx->tx_state != ISOTP_STATE_IDLE) {
        return UDS_STATUS_ERR_BUSY;
    }

    ctx->tx_data        = data;
    ctx->tx_total_len   = length;
    ctx->tx_sent_len    = (uint32_t)0U;
    ctx->tx_sn          = (uint8_t)0U;
    ctx->tx_blocks_sent = (uint8_t)0U;

    if (length <= (uint32_t)7U) {
        /* ---- Classic CAN Single Frame (payload 1-7 bytes) ---- */
        uds_can_frame_t sf;
        uds_status_t    tx_rc;

        (void)memset(&sf, 0, sizeof(sf));
        sf.id      = ctx->tx_can_id;
        sf.dlc     = (uint8_t)(length + (uint32_t)1U);
        sf.data[0] = (uint8_t)length;   /* PCI: SF type (0x0), SF_DL in lower nibble */
        (void)memcpy(&sf.data[1], data, (size_t)length);

#if ISOTP_TX_PADDING
        sf.dlc = (uint8_t)8U;
        isotp_pad_frame(sf.data, (uint8_t)(length + (uint32_t)1U), (uint8_t)8U);
#endif

        tx_rc = can_transport_transmit(ctx->can, &sf);
        if (tx_rc != UDS_STATUS_OK) {
            return UDS_STATUS_ERR_TP_TX_FAILED;
        }

        ctx->tx_sent_len = length;
        /* tx_state remains IDLE — SF is complete. */
        return UDS_STATUS_OK;
    }

#if ISOTP_ENABLE_CAN_FD
    if (ctx->use_fd && (length <= (uint32_t)ISOTP_FD_SF_MAX_PAYLOAD_LEN)) {
        /* ---- [P2-TP-07] CAN FD Single Frame (payload 8-62 bytes) ----
         * ISO 15765-2 §9.8.1: byte 0 = 0x00, byte 1 = SF_DL, data at [2..].
         */
        uds_can_frame_t sf;
        uds_status_t    tx_rc;

        (void)memset(&sf, 0, sizeof(sf));
        sf.id      = ctx->tx_can_id;
        sf.dlc     = (uint8_t)(length + (uint32_t)2U);
        sf.is_fd   = true;
        sf.data[0] = (uint8_t)0x00U;            /* FD SF escape byte */
        sf.data[1] = (uint8_t)length;            /* SF_DL */
        (void)memcpy(&sf.data[2], data, (size_t)length);

#if ISOTP_TX_PADDING
        {
            uint8_t pdlc = isotp_fd_round_dlc((uint8_t)(length + (uint32_t)2U));
            sf.dlc = pdlc;
            isotp_pad_frame(sf.data, (uint8_t)(length + (uint32_t)2U), pdlc);
        }
#endif

        tx_rc = can_transport_transmit(ctx->can, &sf);
        if (tx_rc != UDS_STATUS_OK) {
            return UDS_STATUS_ERR_TP_TX_FAILED;
        }

        ctx->tx_sent_len = length;
        return UDS_STATUS_OK;
    }
#endif /* ISOTP_ENABLE_CAN_FD */

    /* ---- Multi-Frame: send First Frame ---- */
#if ISOTP_ENABLE_CAN_FD
    if (ctx->use_fd && (length > (uint32_t)UDS_MAX_PAYLOAD_LEN)) {
        /* ---- [P2-TP-07] CAN FD FF escape sequence (payload > 4095 bytes) ----
         * ISO 15765-2 §9.8.2: bytes 0-1 = 0x10 0x00, bytes 2-5 = 32-bit FF_DL.
         * First data bytes start at offset 6; up to 58 bytes fit in a 64-byte frame.
         */
        uds_can_frame_t ff;
        uds_status_t    tx_rc;
        uint8_t         first_data;

        first_data = (length > (uint32_t)58U) ? (uint8_t)58U : (uint8_t)length;

        (void)memset(&ff, 0, sizeof(ff));
        ff.id      = ctx->tx_can_id;
        ff.dlc     = (uint8_t)(6U + (uint32_t)first_data);
        ff.is_fd   = true;
        ff.data[0] = (uint8_t)((uint8_t)ISOTP_FRAME_TYPE_FF << (uint8_t)4U); /* 0x10 */
        ff.data[1] = (uint8_t)0x00U;                                          /* escape */
        ff.data[2] = (uint8_t)((length >> 24U) & (uint8_t)0xFFU);
        ff.data[3] = (uint8_t)((length >> 16U) & (uint8_t)0xFFU);
        ff.data[4] = (uint8_t)((length >>  8U) & (uint8_t)0xFFU);
        ff.data[5] = (uint8_t)(length           & (uint8_t)0xFFU);
        (void)memcpy(&ff.data[6], data, (size_t)first_data);

#if ISOTP_TX_PADDING
        {
            uint8_t used = (uint8_t)(6U + (uint32_t)first_data);
            uint8_t pdlc = isotp_fd_round_dlc(used);
            ff.dlc = pdlc;
            isotp_pad_frame(ff.data, used, pdlc);
        }
#endif

        tx_rc = can_transport_transmit(ctx->can, &ff);
        if (tx_rc != UDS_STATUS_OK) {
            return UDS_STATUS_ERR_TP_TX_FAILED;
        }

        ctx->tx_sent_len    = (uint32_t)first_data;
        ctx->tx_sn          = (uint8_t)1U;
        ctx->tx_bs_timer_ms = (uint32_t)ISOTP_TIMEOUT_BS_MS;
        ctx->tx_as_timer_ms = (uint32_t)ISOTP_TIMEOUT_AS_MS;
        ctx->tx_state       = ISOTP_STATE_TX_WAIT_FC;
        return UDS_STATUS_OK;
    }
#endif /* ISOTP_ENABLE_CAN_FD */

    /* ---- Classic CAN FF (12-bit FF_DL, payload 8-4095 bytes) ---- */
    {
        uds_can_frame_t ff;
        uds_status_t    tx_rc;

        (void)memset(&ff, 0, sizeof(ff));
        ff.id      = ctx->tx_can_id;
        ff.dlc     = (uint8_t)8U;
        ff.data[0] = (uint8_t)((uint8_t)((uint8_t)ISOTP_FRAME_TYPE_FF << (uint8_t)4U)
                               | (uint8_t)((length >> 8U) & (uint8_t)0x0FU));
        ff.data[1] = (uint8_t)(length & (uint8_t)0xFFU);
        (void)memcpy(&ff.data[2], data, (size_t)6U);
        /* Classic CAN FF fills all 8 bytes (2 PCI + 6 data) — padding not required. */

        tx_rc = can_transport_transmit(ctx->can, &ff);
        if (tx_rc != UDS_STATUS_OK) {
            return UDS_STATUS_ERR_TP_TX_FAILED;
        }

        ctx->tx_sent_len    = (uint32_t)6U;
        ctx->tx_sn          = (uint8_t)1U;

        /* [P2-TP-05] Arm Bs timer — must receive FC within ISOTP_TIMEOUT_BS_MS. */
        ctx->tx_bs_timer_ms = (uint32_t)ISOTP_TIMEOUT_BS_MS;
        ctx->tx_as_timer_ms = (uint32_t)ISOTP_TIMEOUT_AS_MS;
        ctx->tx_state       = ISOTP_STATE_TX_WAIT_FC;
    }

    return UDS_STATUS_OK;
}

uds_status_t isotp_tick_1ms(isotp_ctx_t *ctx)
{
    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    /* --- RX Cr timer (ISO 15765-2 §6.7.6 Table 5) --- */
    if (ctx->rx_state == ISOTP_STATE_RX_WAIT_CF) {
        if (ctx->rx_cr_timer_ms > 0U) {
            ctx->rx_cr_timer_ms--;
        }
        if (ctx->rx_cr_timer_ms == 0U) {
            ctx->rx_state = ISOTP_STATE_ERROR;
            return UDS_STATUS_ERR_TP_TIMEOUT_CR;
        }
    }

    /* --- TX As timer (sender frame confirmation) ---
     *
     * This timer guards the interval between sending an FF/CF and receiving
     * the transport-layer acknowledgement (FC for FF, or the next-block FC
     * for a CF batch). It must only run while the TX state machine is
     * actively waiting — NOT after TX is complete (IDLE) or in ERROR.
     * Running it unconditionally would corrupt tx_state 25 ms after a
     * successful multi-frame send completes.
     */
    if ((ctx->tx_as_timer_ms > 0U) &&
        (ctx->tx_state == ISOTP_STATE_TX_WAIT_FC ||
         ctx->tx_state == ISOTP_STATE_TX_SEND_CF)) {
        ctx->tx_as_timer_ms--;
        if (ctx->tx_as_timer_ms == 0U) {
            ctx->tx_state = ISOTP_STATE_ERROR;
            return UDS_STATUS_ERR_TP_TIMEOUT_AS;
        }
    } else if (ctx->tx_state == ISOTP_STATE_IDLE) {
        /* TX finished — disarm the As timer so it doesn't fire late. */
        ctx->tx_as_timer_ms = 0U;
    }

    /* --- [P2-TP-05] TX Bs timer (wait for FC) --- */
    if (ctx->tx_state == ISOTP_STATE_TX_WAIT_FC) {
        if (ctx->tx_bs_timer_ms > 0U) {
            ctx->tx_bs_timer_ms--;
        }
        if (ctx->tx_bs_timer_ms == 0U) {
            ctx->tx_state = ISOTP_STATE_ERROR;
            return UDS_STATUS_ERR_TP_TIMEOUT_BS;
        }
    }

    /* --- [P2-TP-06] TX STmin pump (send consecutive frames) --- */
    if (ctx->tx_state == ISOTP_STATE_TX_SEND_CF) {
        if (ctx->tx_stmin_timer_ms > 0U) {
            ctx->tx_stmin_timer_ms--;
        }

        if (ctx->tx_stmin_timer_ms == 0U) {
            isotp_tx_pump(ctx);
        }
    }

    return UDS_STATUS_OK;
}

uds_status_t isotp_reset(isotp_ctx_t *ctx)
{
    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    ctx->rx_state          = ISOTP_STATE_IDLE;
    ctx->tx_state          = ISOTP_STATE_IDLE;
    ctx->rx_expected_len   = (uint32_t)0U;
    ctx->rx_received_len   = (uint32_t)0U;
    ctx->rx_expected_sn    = (uint8_t)0U;
    ctx->rx_cr_timer_ms    = 0U;
    ctx->tx_data           = NULL;
    ctx->tx_total_len      = (uint32_t)0U;
    ctx->tx_sent_len       = (uint32_t)0U;
    ctx->tx_sn             = (uint8_t)0U;
    ctx->tx_block_size     = (uint8_t)0U;
    ctx->tx_stmin_ms       = (uint8_t)0U;
    ctx->tx_stmin_timer_ms = 0U;
    ctx->tx_bs_timer_ms    = 0U;
    ctx->tx_as_timer_ms    = 0U;
    ctx->tx_blocks_sent    = (uint8_t)0U;

    return UDS_STATUS_OK;
}

uds_status_t isotp_get_state(
    const isotp_ctx_t *ctx,
    isotp_state_t     *out_state)
{
    if ((ctx == NULL) || (out_state == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!ctx->initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (ctx->tx_state != ISOTP_STATE_IDLE) {
        *out_state = ctx->tx_state;
    } else {
        *out_state = ctx->rx_state;
    }

    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * Internal helper implementations
 * -------------------------------------------------------------------------- */

static uint8_t isotp_get_frame_type(const uds_can_frame_t *frame)
{
    return (uint8_t)((frame->data[0] >> (uint8_t)4U) & (uint8_t)0x0FU);
}

static uds_status_t isotp_send_fc(
    isotp_ctx_t *ctx,
    uint8_t      flow_status,
    uint8_t      block_size,
    uint8_t      stmin)
{
    uds_can_frame_t fc_frame;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    (void)memset(&fc_frame, 0, sizeof(uds_can_frame_t));

    fc_frame.id      = ctx->tx_can_id;
    fc_frame.dlc     = (uint8_t)3U;
    fc_frame.data[0] = (uint8_t)((uint8_t)((uint8_t)ISOTP_FRAME_TYPE_FC << (uint8_t)4U)
                                 | (flow_status & (uint8_t)0x0FU));
    fc_frame.data[1] = block_size;
    fc_frame.data[2] = stmin;

#if ISOTP_TX_PADDING
    fc_frame.dlc = (uint8_t)8U;
    isotp_pad_frame(fc_frame.data, (uint8_t)3U, (uint8_t)8U);
#endif

    return can_transport_transmit(ctx->can, &fc_frame);
}

#if ISOTP_TX_PADDING
#if ISOTP_ENABLE_CAN_FD
static uint8_t isotp_fd_round_dlc(uint8_t min_bytes)
{
    static const uint8_t fd_dlcs[] = { 12U, 16U, 20U, 24U, 32U, 48U, 64U };
    uint8_t i;

    if (min_bytes <= (uint8_t)8U) {
        return (uint8_t)8U;
    }
    for (i = 0U; i < (uint8_t)(sizeof(fd_dlcs) / sizeof(fd_dlcs[0U])); i++) {
        if (min_bytes <= fd_dlcs[i]) {
            return fd_dlcs[i];
        }
    }
    return (uint8_t)64U;
}
#endif /* ISOTP_ENABLE_CAN_FD */

static void isotp_pad_frame(uint8_t *data, uint8_t used, uint8_t total)
{
    uint8_t i;

    for (i = used; i < total; i++) {
        data[i] = (uint8_t)ISOTP_TX_PADDING_BYTE;
    }
}
#endif /* ISOTP_TX_PADDING */

/* [P2-TP-06] ISO 15765-2 Table 14 STmin decode. */
static uint8_t isotp_decode_stmin_ms(uint8_t stmin_raw)
{
    if (stmin_raw <= (uint8_t)0x7FU) {
        /* 0x00–0x7F: value in milliseconds directly (0–127 ms). */
        return stmin_raw;
    }

    if ((stmin_raw >= (uint8_t)0xF1U) && (stmin_raw <= (uint8_t)0xF9U)) {
        /*
         * 0xF1–0xF9: 100–900 µs sub-millisecond range.
         * Round up to 1 ms (the minimum our 1 ms tick can enforce).
         */
        return (uint8_t)1U;
    }

    /* 0x80–0xF0 and 0xFA–0xFF: reserved — treat as 0 ms. */
    return (uint8_t)0U;
}

/* [P2-TP-04] [P2-TP-06] Consecutive Frame TX pump — sends one CF per call. */
static void isotp_tx_pump(isotp_ctx_t *ctx)
{
    uds_can_frame_t  cf;
    uint32_t         remaining;
    uint8_t          cf_data_len;
    uds_status_t     tx_rc;

    if (ctx->tx_state != ISOTP_STATE_TX_SEND_CF) {
        return;
    }

    if (ctx->tx_data == NULL) {
        ctx->tx_state = ISOTP_STATE_ERROR;
        return;
    }

    remaining = ctx->tx_total_len - ctx->tx_sent_len;
    if (remaining == (uint32_t)0U) {
        /* All bytes sent — TX complete. */
        ctx->tx_state       = ISOTP_STATE_IDLE;
        ctx->tx_data        = NULL;
        ctx->tx_as_timer_ms = 0U;  /* disarm — TX done */
        return;
    }

    cf_data_len = (remaining > (uint32_t)7U) ? (uint8_t)7U : (uint8_t)remaining;

    (void)memset(&cf, 0, sizeof(cf));
    cf.id      = ctx->tx_can_id;
    cf.dlc     = (uint8_t)(cf_data_len + (uint8_t)1U);
    cf.data[0] = (uint8_t)((uint8_t)((uint8_t)ISOTP_FRAME_TYPE_CF << (uint8_t)4U)
                           | (ctx->tx_sn & (uint8_t)0x0FU));
    (void)memcpy(&cf.data[1], &ctx->tx_data[ctx->tx_sent_len], (size_t)cf_data_len);

#if ISOTP_TX_PADDING
    cf.dlc = (uint8_t)8U;
    isotp_pad_frame(cf.data, (uint8_t)(cf_data_len + (uint8_t)1U), (uint8_t)8U);
#endif

    tx_rc = can_transport_transmit(ctx->can, &cf);
    if (tx_rc != UDS_STATUS_OK) {
        ctx->tx_state = ISOTP_STATE_ERROR;
        return;
    }

    ctx->tx_sent_len    = ctx->tx_sent_len + (uint32_t)cf_data_len;
    ctx->tx_sn          = (uint8_t)(ctx->tx_sn + (uint8_t)1U);
    ctx->tx_blocks_sent = (uint8_t)(ctx->tx_blocks_sent + (uint8_t)1U);

    /* Check if all bytes have been sent. */
    if (ctx->tx_sent_len >= ctx->tx_total_len) {
        ctx->tx_state    = ISOTP_STATE_IDLE;
        ctx->tx_data     = NULL;
        ctx->tx_bs_timer_ms = 0U;
        return;
    }

    /* [P2-TP-05] Block-size handling:
     * If tx_block_size > 0 and we've sent tx_block_size CFs since last FC,
     * stop sending and wait for next FC (re-enter TX_WAIT_FC). */
    if ((ctx->tx_block_size != (uint8_t)0U) &&
        (ctx->tx_blocks_sent >= ctx->tx_block_size)) {
        ctx->tx_blocks_sent = (uint8_t)0U;
        ctx->tx_bs_timer_ms = (uint32_t)ISOTP_TIMEOUT_BS_MS;
        ctx->tx_state       = ISOTP_STATE_TX_WAIT_FC;
        return;
    }

    /* [P2-TP-06] Reload STmin timer for next CF. */
    ctx->tx_stmin_timer_ms = (uint32_t)ctx->tx_stmin_ms;
}
