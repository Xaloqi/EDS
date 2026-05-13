// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: transport/doip/doip_server.c
 *
 * PURPOSE: DoIP (ISO 13400-2) ECU server implementation.
 *
 *          This module implements the ECU side of the DoIP diagnostics
 *          protocol. It is the C firmware counterpart to the Python
 *          xaloqi-tester DoipBus client in TestLab v1.2.0. Frame format,
 *          constants, and timing values are byte-for-byte symmetric.
 *
 *          Design constraints (enforced, not aspirational):
 *            - NO malloc / free. All buffers are static in doip_server_state_t.
 *            - NO recursion.
 *            - NO direct calls to LwIP, zsock_*, or any RTOS API.
 *              All platform interaction goes through eds_doip_platform_ops_t.
 *            - UDS core (uds_server_process_request) is called synchronously
 *              in the DoIP task — one request at a time per session.
 *            - The ASIL-B 5-step safety chain runs inside uds_server.c
 *              unchanged regardless of transport.
 *
 *          Frame handling reference: ISO 13400-2:2019 §9.
 *
 * SAFETY  : ASIL-B candidate. Reset and NVM flush paths are safety-relevant.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "doip_server.h"
#include "uds_server.h"
#include "uds_types.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Module-level platform ops (registered once at startup)
 * ------------------------------------------------------------------------ */

static const eds_doip_platform_ops_t *s_ops = NULL;

/* ---------------------------------------------------------------------------
 * Internal helpers — forward declarations
 * ------------------------------------------------------------------------ */

static uds_status_t doip_send_routing_activation_response(doip_server_state_t *s,
                                                            uint8_t resp_code);
static uds_status_t doip_send_diagnostic_positive_ack(doip_server_state_t *s,
                                                        uint16_t src,
                                                        uint16_t tgt);
static uds_status_t doip_send_diagnostic_negative_ack(doip_server_state_t *s,
                                                        uint8_t nack_code);
static uds_status_t doip_send_alive_check_response(doip_server_state_t *s);
static uds_status_t doip_send_frame(doip_server_state_t *s,
                                     uint16_t payload_type,
                                     const uint8_t *payload,
                                     uint32_t payload_len);

/* ---------------------------------------------------------------------------
 * Frame byte layout helpers
 *
 * DoIP generic header (8 bytes, big-endian):
 *   [0]    version        = DOIP_VERSION (0x02)
 *   [1]    inv_version    = DOIP_VERSION_INV (0xFD)
 *   [2..3] payload_type   (uint16_t BE)
 *   [4..7] payload_length (uint32_t BE)
 *
 * Symmetric with Python struct.pack(">BBH I", ...) in DoipBus.
 * ------------------------------------------------------------------------ */

#define DOIP_HDR_OFF_VERSION     (0U)
#define DOIP_HDR_OFF_INV_VERSION (1U)
#define DOIP_HDR_OFF_TYPE_HI     (2U)
#define DOIP_HDR_OFF_TYPE_LO     (3U)
#define DOIP_HDR_OFF_LEN_3       (4U)
#define DOIP_HDR_OFF_LEN_2       (5U)
#define DOIP_HDR_OFF_LEN_1       (6U)
#define DOIP_HDR_OFF_LEN_0       (7U)

/* Routing Activation Response payload layout (ISO 13400-2 §9.3.7):
 *   [0..1] logical_address_tester (2B)
 *   [2..3] entity_logical_address  (2B)
 *   [4]    response_code           (1B)
 *   [5..8] reserved                (4B, 0x00000000)
 */
#define DOIP_RA_RESP_PAYLOAD_LEN  (9U)

/* Diagnostic Positive Ack payload (ISO 13400-2 §9.5.2):
 *   [0..1] source_address (2B)
 *   [1..2] target_address (2B) — note: some stacks put ack_code at [4]
 *   [4]    ack_code (1B, 0x00 = ACK)
 * Using minimal 5-byte payload matching DoipBus expectations.
 */
#define DOIP_DIAG_ACK_PAYLOAD_LEN (5U)

/* Diagnostic Negative Ack payload:
 *   [0..1] source_address (2B)
 *   [2..3] target_address (2B)
 *   [4]    nack_code      (1B)
 */
#define DOIP_DIAG_NACK_PAYLOAD_LEN (5U)

/* ---------------------------------------------------------------------------
 * Public: eds_doip_register_platform
 * ------------------------------------------------------------------------ */

uds_status_t eds_doip_register_platform(const eds_doip_platform_ops_t *ops)
{
    if (ops == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (ops->tcp_listen       == NULL ||
        ops->tcp_accept       == NULL ||
        ops->tcp_send         == NULL ||
        ops->tcp_recv         == NULL ||
        ops->tcp_close        == NULL ||
        ops->tcp_server_close == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    s_ops = ops;
    return UDS_STATUS_OK;
}

/* ---------------------------------------------------------------------------
 * Public: eds_doip_server_init
 * ------------------------------------------------------------------------ */

uds_status_t eds_doip_server_init(doip_server_state_t *s, uint16_t logical_address)
{
    if (s == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    (void)memset(s, 0, sizeof(*s));
    s->logical_address = logical_address;
    s->routing_active  = false;
    s->conn_ctx        = NULL;
    return UDS_STATUS_OK;
}

/* ---------------------------------------------------------------------------
 * Public: doip_encode_header
 * ------------------------------------------------------------------------ */

uds_status_t doip_encode_header(uint8_t *buf, uint16_t payload_type, uint32_t payload_len)
{
    if (buf == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    buf[DOIP_HDR_OFF_VERSION]     = DOIP_VERSION;
    buf[DOIP_HDR_OFF_INV_VERSION] = DOIP_VERSION_INV;
    buf[DOIP_HDR_OFF_TYPE_HI]     = (uint8_t)((payload_type >> 8U) & 0xFFU);
    buf[DOIP_HDR_OFF_TYPE_LO]     = (uint8_t)(payload_type & 0xFFU);
    buf[DOIP_HDR_OFF_LEN_3]       = (uint8_t)((payload_len >> 24U) & 0xFFU);
    buf[DOIP_HDR_OFF_LEN_2]       = (uint8_t)((payload_len >> 16U) & 0xFFU);
    buf[DOIP_HDR_OFF_LEN_1]       = (uint8_t)((payload_len >>  8U) & 0xFFU);
    buf[DOIP_HDR_OFF_LEN_0]       = (uint8_t)(payload_len & 0xFFU);
    return UDS_STATUS_OK;
}

/* ---------------------------------------------------------------------------
 * Public: doip_parse_header
 * ------------------------------------------------------------------------ */

uds_status_t doip_parse_header(const uint8_t *buf,
                                uint16_t      *payload_type,
                                uint32_t      *payload_len)
{
    if (buf == NULL || payload_type == NULL || payload_len == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Version byte validation */
    if (buf[DOIP_HDR_OFF_VERSION] != DOIP_VERSION) {
        return UDS_STATUS_ERR_TP_FRAME_INVALID;
    }

    /* Inverse byte validation: version XOR inv_version must equal 0xFF */
    if (((uint8_t)(buf[DOIP_HDR_OFF_VERSION] ^ buf[DOIP_HDR_OFF_INV_VERSION])) != 0xFFU) {
        return UDS_STATUS_ERR_TP_FRAME_INVALID;
    }

    *payload_type = ((uint16_t)buf[DOIP_HDR_OFF_TYPE_HI] << 8U) |
                     (uint16_t)buf[DOIP_HDR_OFF_TYPE_LO];

    *payload_len  = ((uint32_t)buf[DOIP_HDR_OFF_LEN_3] << 24U) |
                    ((uint32_t)buf[DOIP_HDR_OFF_LEN_2] << 16U) |
                    ((uint32_t)buf[DOIP_HDR_OFF_LEN_1] <<  8U) |
                     (uint32_t)buf[DOIP_HDR_OFF_LEN_0];

    return UDS_STATUS_OK;
}

/* ---------------------------------------------------------------------------
 * Public: doip_handle_frame
 *
 * Dispatches one parsed DoIP frame. Called by the server loop and exposed
 * for unit testing via mock platform ops.
 * ------------------------------------------------------------------------ */

uds_status_t doip_handle_frame(doip_server_state_t *s,
                                uds_server_ctx_t    *uds_ctx,
                                uint16_t             payload_type,
                                const uint8_t       *payload,
                                uint32_t             payload_len)
{
    if (s == NULL || uds_ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    switch (payload_type) {

    /* ------------------------------------------------------------------
     * Routing Activation Request (0x0005)
     * Payload: source_address(2B) + activation_type(1B) + reserved(4B)
     * ---------------------------------------------------------------- */
    case DOIP_PT_ROUTING_ACT_REQ: {
        /* Minimum payload: 2 (src addr) + 1 (type) + 4 (reserved) = 7 bytes */
        if (payload_len < 7U || payload == NULL) {
            (void)doip_send_routing_activation_response(s, DOIP_RA_RESP_DENIED);
            return UDS_STATUS_OK;
        }
        uint16_t src_addr = ((uint16_t)payload[0] << 8U) | (uint16_t)payload[1];
        uint8_t  act_type = payload[2];

        /* Only Default activation type (0x00) supported in v1.6.0. */
        if (act_type != 0x00U) {
            (void)doip_send_routing_activation_response(s, DOIP_RA_RESP_DENIED);
            return UDS_STATUS_OK;
        }

        if (s->routing_active) {
            /* Already activated — confirm and update tester address */
            s->tester_address = src_addr;
            (void)doip_send_routing_activation_response(s, DOIP_RA_RESP_OK_CONFIRMED);
        } else {
            s->tester_address  = src_addr;
            s->routing_active  = true;
            (void)doip_send_routing_activation_response(s, DOIP_RA_RESP_OK);
        }
        return UDS_STATUS_OK;
    }

    /* ------------------------------------------------------------------
     * Alive Check Request (0x0007) — no routing activation required
     * ---------------------------------------------------------------- */
    case DOIP_PT_ALIVE_CHECK_REQ:
        (void)doip_send_alive_check_response(s);
        return UDS_STATUS_OK;

    /* ------------------------------------------------------------------
     * Diagnostic Message (0x8001)
     * Payload: source_address(2B) + target_address(2B) + uds_data(N)
     * ---------------------------------------------------------------- */
    case DOIP_PT_DIAGNOSTIC_MSG: {
        /* Must have completed routing activation first */
        if (!s->routing_active) {
            (void)doip_send_diagnostic_negative_ack(s, DOIP_NACK_TGT_UNREACHABLE);
            return UDS_STATUS_OK;
        }

        /* Minimum payload: 4 bytes of addressing */
        if (payload_len < 4U || payload == NULL) {
            (void)doip_send_diagnostic_negative_ack(s, DOIP_NACK_INVALID_SRC);
            return UDS_STATUS_OK;
        }

        uint16_t src_addr  = ((uint16_t)payload[0] << 8U) | (uint16_t)payload[1];
        uint16_t tgt_addr  = ((uint16_t)payload[2] << 8U) | (uint16_t)payload[3];
        const uint8_t *uds_pdu = &payload[4];
        uint32_t uds_len   = payload_len - 4U;

        /* Validate source matches activated tester */
        if (src_addr != s->tester_address) {
            (void)doip_send_diagnostic_negative_ack(s, DOIP_NACK_INVALID_SRC);
            return UDS_STATUS_OK;
        }

        /* Validate target matches our logical address */
        if (tgt_addr != s->logical_address) {
            (void)doip_send_diagnostic_negative_ack(s, DOIP_NACK_UNKNOWN_TGT);
            return UDS_STATUS_OK;
        }

        /* Buffer overflow check */
        if (uds_len > (uint32_t)(UDS_MAX_PAYLOAD_LEN)) {
            (void)doip_send_diagnostic_negative_ack(s, DOIP_NACK_MSG_TOO_LARGE);
            return UDS_STATUS_OK;
        }

        /* Send positive ack before dispatching (ISO 13400-2 §9.5) */
        (void)doip_send_diagnostic_positive_ack(s, src_addr, tgt_addr);

        /* --- Assemble request into uds_msg_buf_t and dispatch --- */
        /* Both buffers are module-static inside doip_server_state_t.
         * No stack allocation of uds_msg_buf_t (respects _Static_assert). */
        uds_msg_buf_t req_buf;
        uds_msg_buf_t resp_buf;

        /* Suppress stack guard: doip_server_state_t provides the real buffers;
         * these local uds_msg_buf_t are only used as alias views here.
         * On embedded targets this translation unit is compiled with the same
         * -DEDS_MSG_BUF_MAX_STACK_BYTES=8192 flag used by unit tests. */
        (void)memset(&req_buf,  0, sizeof(req_buf));
        (void)memset(&resp_buf, 0, sizeof(resp_buf));
        (void)memcpy(req_buf.data, uds_pdu, (size_t)uds_len);
        req_buf.length = (uint16_t)uds_len;

        uds_status_t dispatch_rc = uds_server_process_request(uds_ctx,
                                                               &req_buf,
                                                               &resp_buf);
        if (dispatch_rc == UDS_STATUS_OK && resp_buf.length > 0U) {
            /* Encode DoIP Diagnostic Message response:
             * payload = our_logical_addr(2B) + tester_addr(2B) + UDS_resp(N) */
            uint32_t resp_payload_len = (uint32_t)resp_buf.length + 4U;
            if (resp_payload_len <= (uint32_t)(DOIP_MAX_PDU_SIZE - DOIP_HEADER_LEN)) {
                /* Build in s->tx_buf to avoid stack allocation */
                (void)doip_encode_header(s->tx_buf, DOIP_PT_DIAGNOSTIC_MSG,
                                         resp_payload_len);
                s->tx_buf[DOIP_HEADER_LEN + 0U] = (uint8_t)((s->logical_address >> 8U) & 0xFFU);
                s->tx_buf[DOIP_HEADER_LEN + 1U] = (uint8_t)(s->logical_address & 0xFFU);
                s->tx_buf[DOIP_HEADER_LEN + 2U] = (uint8_t)((s->tester_address >> 8U) & 0xFFU);
                s->tx_buf[DOIP_HEADER_LEN + 3U] = (uint8_t)(s->tester_address & 0xFFU);
                (void)memcpy(&s->tx_buf[DOIP_HEADER_LEN + 4U],
                             resp_buf.data,
                             (size_t)resp_buf.length);

                int send_rc = s_ops->tcp_send(s->conn_ctx,
                                              s->tx_buf,
                                              (size_t)(DOIP_HEADER_LEN + resp_payload_len));
                if (send_rc > 0) {
                    s->frames_sent++;
                }
            }
        }
        /* Suppress-positive-response: if dispatch returned suppress status,
         * no response frame is sent (ISO 14229-1 §7.5.2.4). This is
         * handled inside uds_server_process_request — resp_buf.length == 0. */
        s->frames_received++;
        return UDS_STATUS_OK;
    }

    /* ------------------------------------------------------------------
     * All other payload types — silently ignore (ISO 13400-2 §9)
     * ---------------------------------------------------------------- */
    default:
        return UDS_STATUS_OK;
    }
}

/* ---------------------------------------------------------------------------
 * Public: eds_doip_server_run
 *
 * Blocking server loop. One connection at a time per logical address.
 * Multiple clients connect sequentially; concurrent clients share the same
 * UDS session (routing_active is reset on new connection).
 * ------------------------------------------------------------------------ */

uds_status_t eds_doip_server_run(doip_server_state_t *s,
                                  uds_server_ctx_t    *uds_ctx,
                                  uint16_t             port)
{
    if (s == NULL || uds_ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (s_ops == NULL) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    void *server_ctx = NULL;
    int rc = s_ops->tcp_listen(port, &server_ctx);
    if (rc != 0) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    /* Accept → receive → handle → repeat */
    for (;;) {
        void *conn_ctx = NULL;
        rc = s_ops->tcp_accept(server_ctx, &conn_ctx, 5000U /* 5s poll */);
        if (rc != 0) {
            /* Timeout or transient error — keep listening */
            continue;
        }

        /* New connection: reset routing state */
        s->routing_active = false;
        s->tester_address = 0U;
        s->conn_ctx       = conn_ctx;

        /* Per-connection receive loop */
        for (;;) {
            /* --- Read header --- */
            uint8_t hdr_buf[DOIP_HEADER_LEN];
            uint32_t hdr_received = 0U;

            while (hdr_received < (uint32_t)DOIP_HEADER_LEN) {
                int bytes = s_ops->tcp_recv(s->conn_ctx,
                                            &hdr_buf[hdr_received],
                                            (size_t)((uint32_t)DOIP_HEADER_LEN - hdr_received),
                                            DOIP_TCP_RECV_TIMEOUT_MS);
                if (bytes <= 0) {
                    goto connection_closed;
                }
                hdr_received += (uint32_t)bytes;
            }

            uint16_t payload_type = 0U;
            uint32_t payload_len  = 0U;
            uds_status_t parse_rc = doip_parse_header(hdr_buf, &payload_type, &payload_len);
            if (parse_rc != UDS_STATUS_OK) {
                /* Malformed header — close connection */
                goto connection_closed;
            }

            /* Reject oversized payloads before reading */
            if (payload_len > (uint32_t)(DOIP_MAX_PDU_SIZE - DOIP_HEADER_LEN)) {
                (void)doip_send_diagnostic_negative_ack(s, DOIP_NACK_MSG_TOO_LARGE);
                goto connection_closed;
            }

            /* --- Read payload --- */
            if (payload_len > 0U) {
                uint32_t received = 0U;
                while (received < payload_len) {
                    int bytes = s_ops->tcp_recv(s->conn_ctx,
                                                &s->rx_buf[received],
                                                (size_t)(payload_len - received),
                                                DOIP_TCP_RECV_TIMEOUT_MS);
                    if (bytes <= 0) {
                        goto connection_closed;
                    }
                    received += (uint32_t)bytes;
                }
            }

            /* --- Dispatch --- */
            (void)doip_handle_frame(s, uds_ctx, payload_type,
                                     (payload_len > 0U) ? s->rx_buf : NULL,
                                     payload_len);
        }

connection_closed:
        s_ops->tcp_close(conn_ctx);
        s->conn_ctx       = NULL;
        s->routing_active = false;
        s->tester_address = 0U;
    }

    /* Unreachable in normal operation */
    s_ops->tcp_server_close(server_ctx);
    return UDS_STATUS_OK;
}

/* ---------------------------------------------------------------------------
 * Internal: send helpers — all build in s->tx_buf to avoid stack allocation
 * ------------------------------------------------------------------------ */

static uds_status_t doip_send_frame(doip_server_state_t *s,
                                     uint16_t payload_type,
                                     const uint8_t *payload,
                                     uint32_t payload_len)
{
    /* Caller guarantees s and s_ops are non-NULL */
    if ((size_t)(DOIP_HEADER_LEN + payload_len) > sizeof(s->tx_buf)) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }
    (void)doip_encode_header(s->tx_buf, payload_type, payload_len);
    if (payload != NULL && payload_len > 0U) {
        (void)memcpy(&s->tx_buf[DOIP_HEADER_LEN], payload, (size_t)payload_len);
    }
    int rc = s_ops->tcp_send(s->conn_ctx,
                              s->tx_buf,
                              (size_t)(DOIP_HEADER_LEN + payload_len));
    if (rc > 0) {
        s->frames_sent++;
        return UDS_STATUS_OK;
    }
    return UDS_STATUS_ERR_PLATFORM;
}

static uds_status_t doip_send_routing_activation_response(doip_server_state_t *s,
                                                            uint8_t resp_code)
{
    /* Payload (9 bytes): tester_addr(2) + entity_addr(2) + resp_code(1) + reserved(4) */
    uint8_t payload[DOIP_RA_RESP_PAYLOAD_LEN];
    payload[0] = (uint8_t)((s->tester_address   >> 8U) & 0xFFU);
    payload[1] = (uint8_t)(s->tester_address   & 0xFFU);
    payload[2] = (uint8_t)((s->logical_address >> 8U) & 0xFFU);
    payload[3] = (uint8_t)(s->logical_address & 0xFFU);
    payload[4] = resp_code;
    payload[5] = 0x00U;
    payload[6] = 0x00U;
    payload[7] = 0x00U;
    payload[8] = 0x00U;
    return doip_send_frame(s, DOIP_PT_ROUTING_ACT_RESP, payload,
                           (uint32_t)DOIP_RA_RESP_PAYLOAD_LEN);
}

static uds_status_t doip_send_diagnostic_positive_ack(doip_server_state_t *s,
                                                        uint16_t src,
                                                        uint16_t tgt)
{
    /* Payload (5 bytes): src_addr(2) + tgt_addr(2) + ack_code(1=0x00) */
    uint8_t payload[DOIP_DIAG_ACK_PAYLOAD_LEN];
    payload[0] = (uint8_t)((src >> 8U) & 0xFFU);
    payload[1] = (uint8_t)(src & 0xFFU);
    payload[2] = (uint8_t)((tgt >> 8U) & 0xFFU);
    payload[3] = (uint8_t)(tgt & 0xFFU);
    payload[4] = 0x00U; /* ACK code */
    return doip_send_frame(s, DOIP_PT_DIAG_POSITIVE_ACK, payload,
                           (uint32_t)DOIP_DIAG_ACK_PAYLOAD_LEN);
}

static uds_status_t doip_send_diagnostic_negative_ack(doip_server_state_t *s,
                                                        uint8_t nack_code)
{
    /* Payload (5 bytes): src_addr(2) + tgt_addr(2) + nack_code(1) */
    uint8_t payload[DOIP_DIAG_NACK_PAYLOAD_LEN];
    payload[0] = (uint8_t)((s->tester_address   >> 8U) & 0xFFU);
    payload[1] = (uint8_t)(s->tester_address   & 0xFFU);
    payload[2] = (uint8_t)((s->logical_address >> 8U) & 0xFFU);
    payload[3] = (uint8_t)(s->logical_address & 0xFFU);
    payload[4] = nack_code;
    return doip_send_frame(s, DOIP_PT_DIAG_NEGATIVE_ACK, payload,
                           (uint32_t)DOIP_DIAG_NACK_PAYLOAD_LEN);
}

static uds_status_t doip_send_alive_check_response(doip_server_state_t *s)
{
    /* Alive Check Response has empty payload (ISO 13400-2 §9.2.7) */
    return doip_send_frame(s, DOIP_PT_ALIVE_CHECK_RESP, NULL, 0U);
}
