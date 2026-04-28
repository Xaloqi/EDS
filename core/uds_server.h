// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_server.h
 *
 * PURPOSE: Public API for the UDS server — the central dispatcher that
 *          receives assembled PDUs from the transport layer, routes them
 *          to the appropriate service handler, and returns the response.
 *
 * SAFETY  : Safety-relevant interface. Changes require safety review.
 *           ASIL-B candidate per ISO 26262-6.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef UDS_SERVER_H
#define UDS_SERVER_H

#include "uds_types.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_access_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Configuration constants (override via prj.conf / Kconfig)
 * -------------------------------------------------------------------------- */

/** Maximum number of registered service handlers. */
#ifndef UDS_SERVER_MAX_HANDLERS
#define UDS_SERVER_MAX_HANDLERS (16U)
#endif

/* --------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------- */

/** Opaque server context. Callers must not access members directly. */
typedef struct uds_server_ctx uds_server_ctx_t;

/* --------------------------------------------------------------------------
 * Service handler callback signature
 *
 * SAFETY: Timing-critical — must complete within P2server timeout.
 * -------------------------------------------------------------------------- */

/**
 * @brief Prototype for a UDS service handler callback.
 *
 * @param[in]  ctx         Pointer to server context.
 * @param[in]  req         Pointer to the request message buffer (immutable).
 * @param[out] resp        Pointer to the response message buffer to populate.
 *
 * @return UDS_STATUS_OK if positive response is ready in resp.
 * @return UDS_STATUS_ERR_* if a negative response or no response should be sent.
 */
typedef uds_status_t (*uds_service_handler_fn)(
    uds_server_ctx_t       *ctx,
    const uds_msg_buf_t    *req,
    uds_msg_buf_t          *resp
);

/* --------------------------------------------------------------------------
 * Service registration descriptor
 * -------------------------------------------------------------------------- */

/**
 * @brief Descriptor binding a service ID to its handler callback.
 */
typedef struct uds_service_entry {
    uint8_t                 service_id;    /**< UDS service identifier byte. */
    uds_service_handler_fn  handler;       /**< Pointer to service handler function. */
    bool                    suppress_pos_response_supported; /**< Whether suppressPosRspMsgIndicationBit is observed. */
} uds_service_entry_t;

/* --------------------------------------------------------------------------
 * Server configuration structure
 * -------------------------------------------------------------------------- */

/**
 * @brief Static configuration block for the UDS server.
 *
 * Must be populated by the application prior to calling uds_server_init().
 * All pointer fields must remain valid for the lifetime of the server instance.
 */
typedef struct uds_server_cfg {
    uint32_t                    p2_server_max_ms;     /**< P2Server_max timeout (ms). */
    uint32_t                    p2_star_server_max_ms;/**< P2*Server_max timeout (ms). */
    uds_session_ctx_t          *session_ctx;          /**< Pointer to session context (required). */
    uds_security_ctx_t         *security_ctx;         /**< Pointer to security context (required). */
    const uds_service_entry_t  *service_table;        /**< Pointer to service registration table. */
    uint8_t                     service_table_count;  /**< Number of entries in service_table[]. */

    /**
     * @brief [P5-ACL-01] Optional data-driven access rights table.
     *
     * When non-NULL, the server dispatcher uses this table to check session
     * and security constraints before dispatching any service. When NULL,
     * the built-in default table (uds_access_table_get_default()) is used.
     *
     * OEMs supply a custom table to restrict or relax access on any SID
     * without modifying stack source files.
     *
     * Must remain valid for the lifetime of the server context.
     */
    const uds_access_entry_t   *access_table;         /**< [P5-ACL-01] Access rights table. NULL = use default. */
    uint8_t                     access_table_count;   /**< [P5-ACL-01] Number of entries in access_table[]. 0 = use default. */
} uds_server_cfg_t;

/* --------------------------------------------------------------------------
 * Opaque server context (definition is internal to uds_server.c)
 * -------------------------------------------------------------------------- */

/**
 * @brief UDS server context — full definition exposed for static allocation.
 *
 * SAFETY: Members must not be accessed directly by external modules.
 *         All interaction must occur through the public API.
 */
struct uds_server_ctx {
    bool                initialized;                            /**< Initialization guard. */
    uds_server_cfg_t    cfg;                                    /**< Cached configuration copy. */
    uds_msg_buf_t       tx_buf;                                 /**< Static TX response buffer. */
    uds_msg_buf_t       rx_buf;                                 /**< Static RX request buffer. */
    uint32_t            request_count;                          /**< Total processed request counter. */
    uint32_t            negative_response_count;                /**< Negative response counter. */
    uint8_t             pending_reset_type;                     /**< [P2-0x11-01] Non-zero after 0x11; deferred reset type. */
};

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialize the UDS server module.
 *
 * Must be called exactly once before any other uds_server_* API.
 * Idempotent initialization is rejected with UDS_STATUS_ERR_ALREADY_INITIALIZED.
 *
 * @param[out] ctx  Pointer to caller-allocated server context.
 * @param[in]  cfg  Pointer to server configuration structure.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if ctx or cfg is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if cfg fields are invalid.
 * @return UDS_STATUS_ERR_ALREADY_INITIALIZED if already initialized.
 *
 * @note SAFETY: Must be called from initialization context, not from ISR.
 */
uds_status_t uds_server_init(
    uds_server_ctx_t       *ctx,
    const uds_server_cfg_t *cfg
);

/**
 * @brief Process a single incoming UDS request PDU.
 *
 * Dispatches the assembled request to the appropriate service handler.
 * Populates resp with the response PDU. Caller is responsible for
 * transmitting resp via the transport layer.
 *
 * @param[in]  ctx   Pointer to initialized server context.
 * @param[in]  req   Pointer to the assembled request PDU buffer.
 * @param[out] resp  Pointer to the response buffer to populate.
 *
 * @return UDS_STATUS_OK if resp contains a valid response to transmit.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer argument is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if ctx is not initialized.
 * @return UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED if no handler is registered.
 *
 * @note TIMING: Must complete within P2server_max. Caller enforces timer.
 * @note SAFETY: Timing-critical interface — ASIL-B relevant.
 */
uds_status_t uds_server_process_request(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp
);

/**
 * @brief Periodic tick function — must be called at 1 ms resolution.
 *
 * Drives session timeout and security delay timers.
 *
 * @param[in] ctx  Pointer to initialized server context.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if ctx is NULL.
 *
 * @note TIMING: Timing-critical — must be called at consistent 1 ms intervals.
 * @note SAFETY: ASIL-B relevant — drives S3server session timeout.
 */
uds_status_t uds_server_tick_1ms(uds_server_ctx_t *ctx);

/**
 * @brief Build a standard negative response PDU.
 *
 * Populates resp with a UDS 0x7F negative response message.
 *
 * @param[in]  service_id  SID of the request being rejected.
 * @param[in]  nrc         Negative response code to transmit.
 * @param[out] resp        Pointer to the response buffer to populate.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if resp is NULL.
 */
uds_status_t uds_server_build_negative_response(
    uint8_t        service_id,
    uds_nrc_t      nrc,
    uds_msg_buf_t *resp
);

/**
 * @brief Return current server diagnostic counters.
 *
 * @param[in]  ctx                     Initialized server context.
 * @param[out] out_request_count        Total requests processed.
 * @param[out] out_neg_response_count   Total negative responses issued.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 */
uds_status_t uds_server_get_counters(
    const uds_server_ctx_t *ctx,
    uint32_t               *out_request_count,
    uint32_t               *out_neg_response_count
);

#ifdef __cplusplus
}
#endif

#endif /* UDS_SERVER_H */
