// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: transport/doip/doip_server.h
 *
 * PURPOSE: DoIP (ISO 13400-2) ECU server — public API.
 *
 *          Implements the ECU (entity) side of DoIP diagnostics over TCP/IP.
 *          This is the firmware counterpart to xaloqi-tester DoipBus (Python).
 *
 *          Supported in v1.6.0:
 *            - TCP connection accept (up to DOIP_MAX_CONNECTIONS)
 *            - Routing Activation Request/Response (Default type 0x00)
 *            - UDS DiagnosticMessage (0x8001): receive, dispatch, respond
 *            - DiagnosticMessage Positive Ack (0x8002)
 *            - DiagnosticMessage Negative Ack (0x8003)
 *            - Alive Check Request/Response (0x0007/0x0008)
 *            - Generic header validation (version 0x02, inverse byte)
 *
 *          Not implemented in v1.6.0 (documented, not planned):
 *            - UDP Vehicle Identification / Entity Status / PowerMode
 *            - DoIP Vehicle Announcement
 *            - TLS (plain TCP only)
 *            - Multiple routing activation types (Default=0x00 only)
 *            - IPv6 (IPv4 only)
 *
 *          Frame format is byte-for-byte symmetric with DoipBus in
 *          xaloqi-tester (TestLab v1.2.0). Constants must remain in sync.
 *
 * PLATFORM: Calls only eds_doip_platform_ops_t callbacks — never LwIP or
 *           Zephyr BSD sockets directly. Platform bindings:
 *             transport/doip/zephyr_lwip.c   — Zephyr
 *             transport/doip/freertos_lwip.c — FreeRTOS + LwIP
 *
 * SAFETY  : ASIL-B candidate. No malloc. No recursion. Static buffers.
 * STANDARD: ISO 13400-2:2019. MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef DOIP_SERVER_H
#define DOIP_SERVER_H

#include "uds_types.h"
#include "uds_server.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * DoIP constants (ISO 13400-2:2019)
 * Symmetric with xaloqi-tester transport/doip.py — do not change values.
 * ========================================================================== */

#define DOIP_VERSION            (0x02U)  /**< ISO 13400-2:2019 protocol version. */
#define DOIP_VERSION_INV        (0xFDU)  /**< Inverse byte (0xFF XOR 0x02). */
#define DOIP_HEADER_LEN         (8U)     /**< Generic header size in bytes. */
#define DOIP_PORT               (13400U) /**< Standard TCP port (ISO 13400). */

/** Payload type codes (ISO 13400-2 Table 17). */
#define DOIP_PT_ROUTING_ACT_REQ    (0x0005U)
#define DOIP_PT_ROUTING_ACT_RESP   (0x0006U)
#define DOIP_PT_ALIVE_CHECK_REQ    (0x0007U)
#define DOIP_PT_ALIVE_CHECK_RESP   (0x0008U)
#define DOIP_PT_DIAGNOSTIC_MSG     (0x8001U)
#define DOIP_PT_DIAG_POSITIVE_ACK  (0x8002U)
#define DOIP_PT_DIAG_NEGATIVE_ACK  (0x8003U)

/** Routing activation response codes (ISO 13400-2 Table 25). */
#define DOIP_RA_RESP_DENIED        (0x00U) /**< Denied — unknown source address. */
#define DOIP_RA_RESP_OK            (0x10U) /**< Routing activation successful. */
#define DOIP_RA_RESP_OK_CONFIRMED  (0x11U) /**< Already activated. */

/** Diagnostic negative ack codes (ISO 13400-2 Table 36). */
#define DOIP_NACK_INVALID_SRC      (0x03U) /**< Invalid source address. */
#define DOIP_NACK_UNKNOWN_TGT      (0x04U) /**< Unknown target address. */
#define DOIP_NACK_MSG_TOO_LARGE    (0x05U) /**< Diagnostic message too large. */
#define DOIP_NACK_OUT_OF_MEMORY    (0x06U) /**< Out of memory. */
#define DOIP_NACK_TGT_UNREACHABLE  (0x07U) /**< Target unreachable (not routing-active). */

/* ============================================================================
 * Tuneable limits (override via Kconfig / -D flags)
 * ========================================================================== */

/** Maximum UDS PDU size transported over DoIP (bytes). */
#ifndef DOIP_MAX_PDU_SIZE
#define DOIP_MAX_PDU_SIZE       (4096U)
#endif

/** Maximum concurrent TCP clients accepted. */
#ifndef DOIP_MAX_CONNECTIONS
#define DOIP_MAX_CONNECTIONS    (4U)
#endif

/** TCP receive timeout in ms — P2Server equivalent. */
#ifndef DOIP_TCP_RECV_TIMEOUT_MS
#define DOIP_TCP_RECV_TIMEOUT_MS (150U)
#endif

/** Keepalive period — Alive Check Request sent every N ms if idle. */
#ifndef DOIP_ALIVE_CHECK_PERIOD_MS
#define DOIP_ALIVE_CHECK_PERIOD_MS (500U)
#endif

/* ============================================================================
 * Platform operations — implement one set per RTOS/IP-stack combination.
 *
 * All function pointers must be non-NULL before calling eds_doip_register_platform().
 * A CAN-only build leaves doip_ops un-registered (NULL) — doip_server.c
 * performs a NULL guard before calling any op.
 *
 * Error conventions:
 *   tcp_listen / tcp_accept / tcp_send : return 0 on success, negative on error.
 *   tcp_recv  : return bytes received (>0), 0 if connection closed cleanly,
 *               negative on error/timeout.
 *   tcp_close / tcp_server_close : void, best-effort.
 *
 * All pointer arguments are non-NULL unless otherwise noted.
 * ========================================================================== */

typedef struct eds_doip_platform_ops {
    /**
     * @brief Open a TCP server socket and begin listening.
     *
     * @param[in]  port        TCP port to listen on (typically DOIP_PORT).
     * @param[out] server_ctx  Opaque handle for this server socket.
     * @return 0 on success, negative errno on failure.
     */
    int (*tcp_listen)(uint16_t port, void **server_ctx);

    /**
     * @brief Accept the next incoming client connection.
     *
     * Blocks until a client connects or timeout_ms elapses.
     *
     * @param[in]  server_ctx  Server socket handle from tcp_listen().
     * @param[out] conn_ctx    Opaque handle for the accepted connection.
     * @param[in]  timeout_ms  Maximum wait time in milliseconds.
     * @return 0 on success, negative on timeout or error.
     */
    int (*tcp_accept)(void *server_ctx, void **conn_ctx, uint32_t timeout_ms);

    /**
     * @brief Send data on an established TCP connection.
     *
     * @param[in] conn_ctx  Connection handle from tcp_accept().
     * @param[in] data      Bytes to transmit.
     * @param[in] len       Number of bytes.
     * @return Bytes sent (>0) or negative errno.
     */
    int (*tcp_send)(void *conn_ctx, const uint8_t *data, size_t len);

    /**
     * @brief Receive data from a TCP connection.
     *
     * @param[in]  conn_ctx    Connection handle.
     * @param[out] buf         Destination buffer.
     * @param[in]  buf_len     Buffer capacity.
     * @param[in]  timeout_ms  Receive timeout in milliseconds.
     * @return Bytes received (>0), 0 if connection closed, negative on error/timeout.
     */
    int (*tcp_recv)(void *conn_ctx, uint8_t *buf, size_t buf_len, uint32_t timeout_ms);

    /**
     * @brief Close a client connection.
     *
     * @param[in] conn_ctx  Connection handle to close.
     */
    void (*tcp_close)(void *conn_ctx);

    /**
     * @brief Close the server (listening) socket.
     *
     * @param[in] server_ctx  Server socket handle to close.
     */
    void (*tcp_server_close)(void *server_ctx);
} eds_doip_platform_ops_t;

/* ============================================================================
 * Internal server state (one instance per ECU logical address).
 *
 * SAFETY: Caller must not modify members directly. Access only through API.
 * ========================================================================== */

typedef struct doip_server_state {
    uint16_t logical_address;     /**< This ECU's logical address. */
    uint16_t tester_address;      /**< Tester logical address (from routing activation). */
    bool     routing_active;      /**< True after successful routing activation. */
    void    *conn_ctx;            /**< Current active connection handle (NULL if none). */
    uint8_t  rx_buf[DOIP_MAX_PDU_SIZE]; /**< Static receive buffer — no malloc. */
    uint8_t  tx_buf[DOIP_MAX_PDU_SIZE]; /**< Static transmit buffer — no malloc. */
    uint32_t frames_received;    /**< Diagnostic counter: total frames received. */
    uint32_t frames_sent;        /**< Diagnostic counter: total frames sent. */
} doip_server_state_t;

/* ============================================================================
 * Public API
 * ========================================================================== */

/**
 * @brief Register platform TCP operations with the DoIP server.
 *
 * Must be called once before eds_doip_server_run().
 * The ops structure must remain valid for the lifetime of the server.
 *
 * @param[in] ops  Non-NULL pointer to platform ops. All function pointers
 *                 within ops must be non-NULL.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if ops or any function pointer is NULL.
 */
uds_status_t eds_doip_register_platform(const eds_doip_platform_ops_t *ops);

/**
 * @brief Initialise the DoIP server state for one ECU logical address.
 *
 * @param[out] s               Caller-allocated server state (static storage).
 * @param[in]  logical_address This ECU's DoIP logical address.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if s is NULL.
 */
uds_status_t eds_doip_server_init(doip_server_state_t *s, uint16_t logical_address);

/**
 * @brief Main server loop — blocks indefinitely, never returns in normal operation.
 *
 * Accepts TCP connections, performs routing activation, and dispatches
 * UDS DiagnosticMessage frames to the UDS server core via uds_server_ctx.
 *
 * Must be called from a dedicated RTOS task (Zephyr thread / FreeRTOS task).
 *
 * @param[in] s           Initialised DoIP server state.
 * @param[in] uds_ctx     Initialised UDS server context (from uds_server_init()).
 * @param[in] port        TCP port to listen on. Pass DOIP_PORT for standard.
 *
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if eds_doip_register_platform() not called.
 * @return UDS_STATUS_ERR_NULL_PTR if s or uds_ctx is NULL.
 * @return UDS_STATUS_ERR_PLATFORM if tcp_listen() fails.
 * @note   Returns only on fatal platform error. Normal operation: never returns.
 */
uds_status_t eds_doip_server_run(doip_server_state_t *s,
                                  uds_server_ctx_t    *uds_ctx,
                                  uint16_t             port);

/* ============================================================================
 * Frame encode/decode — exposed for unit testing.
 *
 * These functions operate on caller-provided buffers. No dynamic allocation.
 * ========================================================================== */

/**
 * @brief Encode a DoIP generic header into buf[0..7].
 *
 * @param[out] buf           Output buffer (must be >= DOIP_HEADER_LEN bytes).
 * @param[in]  payload_type  Payload type code (DOIP_PT_*).
 * @param[in]  payload_len   Length of the payload that follows.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf is NULL.
 */
uds_status_t doip_encode_header(uint8_t *buf, uint16_t payload_type, uint32_t payload_len);

/**
 * @brief Parse and validate a DoIP generic header from buf[0..7].
 *
 * Validates version byte, inverse byte, and that buf is long enough.
 *
 * @param[in]  buf           Input buffer (must be >= DOIP_HEADER_LEN bytes).
 * @param[out] payload_type  Decoded payload type.
 * @param[out] payload_len   Decoded payload length.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_TP_FRAME_INVALID if version or inverse byte is wrong.
 */
uds_status_t doip_parse_header(const uint8_t *buf,
                                uint16_t      *payload_type,
                                uint32_t      *payload_len);

/**
 * @brief Dispatch a parsed DoIP frame to the appropriate handler.
 *
 * Called internally by eds_doip_server_run(). Exposed for unit testing.
 * The platform send callback is invoked on s->conn_ctx for any response frames.
 *
 * @param[in] s            DoIP server state.
 * @param[in] uds_ctx      UDS server context.
 * @param[in] payload_type Parsed payload type.
 * @param[in] payload      Pointer to payload bytes (may be NULL if payload_len == 0).
 * @param[in] payload_len  Number of valid bytes in payload.
 *
 * @return UDS_STATUS_OK if frame was handled (even if a NACK was sent).
 * @return UDS_STATUS_ERR_NULL_PTR if s or uds_ctx is NULL.
 */
uds_status_t doip_handle_frame(doip_server_state_t *s,
                                uds_server_ctx_t    *uds_ctx,
                                uint16_t             payload_type,
                                const uint8_t       *payload,
                                uint32_t             payload_len);

#ifdef __cplusplus
}
#endif

#endif /* DOIP_SERVER_H */
