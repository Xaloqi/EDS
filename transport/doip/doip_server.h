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
 *          Supported in v1.7.0:
 *            - TCP connection accept (one active connection at a time,
 *              served serially — see DOIP_MAX_CONNECTIONS below)
 *            - Routing Activation Request/Response (Default type 0x00)
 *            - UDS DiagnosticMessage (0x8001): receive, dispatch, respond
 *            - DiagnosticMessage Positive Ack (0x8002)
 *            - DiagnosticMessage Negative Ack (0x8003)
 *            - Alive Check Request/Response (0x0007/0x0008)
 *            - Generic header validation (version 0x02, inverse byte)
 *
 *          [EDS#392] Vehicle discovery over UDP, when the platform ops
 *          include UDP (on by default; see eds_doip_set_discovery()):
 *            - Vehicle Identification Request 0x0001 / by EID 0x0002 /
 *              by VIN 0x0003 -> response 0x0004
 *            - Vehicle Announcement 0x0004 (A_DoIP_Announce_* defaults)
 *            - Entity Status 0x4001/0x4002, Power Mode 0x4003/0x4004
 *            - Generic header NACK 0x0000 for malformed datagrams
 *
 *          Not implemented (documented, not planned):
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

/** Protocol version byte. 0x02 is the ISO 13400-2:2012 value (2019 is 0x03),
 * what EDS has always sent and what testers default to; parsing and the
 * message layout follow the 2019 text. [EDS#571] */
#define DOIP_VERSION            (0x02U)
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

/* [EDS#392] Vehicle discovery over UDP (ISO 13400-2 payload types). */
#define DOIP_PT_GENERIC_NACK       (0x0000U) /**< Generic DoIP header negative ack. */
#define DOIP_PT_VEHICLE_ID_REQ     (0x0001U) /**< Vehicle identification request. */
#define DOIP_PT_VEHICLE_ID_REQ_EID (0x0002U) /**< ... with EID (6 bytes). */
#define DOIP_PT_VEHICLE_ID_REQ_VIN (0x0003U) /**< ... with VIN (17 bytes). */
#define DOIP_PT_VEHICLE_ANNOUNCE   (0x0004U) /**< Vehicle announcement / identification response. */
#define DOIP_PT_ENTITY_STATUS_REQ  (0x4001U)
#define DOIP_PT_ENTITY_STATUS_RESP (0x4002U)
#define DOIP_PT_POWER_MODE_REQ     (0x4003U) /**< Diagnostic power mode information request. */
#define DOIP_PT_POWER_MODE_RESP    (0x4004U)

/** [EDS#392] Protocol version a tester may put in a vehicle identification
 * request (0x0001-0x0003) instead of its own, per ISO 13400-2. The Eclipse
 * OpenSOVD CDA sends it. Accepted on UDP for those three types only; TCP
 * stays strict. */
#define DOIP_VERSION_DEFAULT       (0xFFU)

/** [EDS#392] Generic DoIP header negative acknowledge codes (ISO 13400-2). */
#define DOIP_HDR_NACK_INCORRECT_PATTERN  (0x00U)
#define DOIP_HDR_NACK_UNKNOWN_TYPE       (0x01U)
#define DOIP_HDR_NACK_MSG_TOO_LARGE      (0x02U)
#define DOIP_HDR_NACK_OUT_OF_MEMORY      (0x03U)
#define DOIP_HDR_NACK_INVALID_LENGTH     (0x04U)

/** [EDS#392] Field sizes of the vehicle identification response. */
#define DOIP_VIN_LEN               (17U)
#define DOIP_EID_LEN               (6U)
#define DOIP_GID_LEN               (6U)

/** [EDS#392] Further action byte: no further action required. */
#define DOIP_FURTHER_ACTION_NONE   (0x00U)

/** [EDS#392] VIN/GID sync status: incomplete, VIN and GID not synchronized.
 * Sent (as the optional 33rd byte) only when the VIN is not available. */
#define DOIP_SYNC_STATUS_INCOMPLETE (0x10U)

/** [EDS#392] Entity status node type: DoIP node (not a gateway). */
#define DOIP_NODE_TYPE_NODE        (0x01U)

/** [EDS#392] Diagnostic power mode values. */
#define DOIP_POWER_MODE_NOT_READY     (0x00U)
#define DOIP_POWER_MODE_READY         (0x01U)
#define DOIP_POWER_MODE_NOT_SUPPORTED (0x02U)

/* ============================================================================
 * Tuneable limits (override via Kconfig / -D flags)
 * ========================================================================== */

/** Maximum UDS PDU size transported over DoIP (bytes). */
#ifndef DOIP_MAX_PDU_SIZE
#define DOIP_MAX_PDU_SIZE       (4096U)
#endif

/** TCP listen() backlog — the number of pending inbound connections the
 * platform accept queue will hold. This is NOT the number of connections
 * served concurrently: eds_doip_server_run() handles one connection at a
 * time (serial model, see eds_doip_server_run() below) — a queued client
 * waits until the current one closes before tcp_accept() picks it up. */
#ifndef DOIP_MAX_CONNECTIONS
#define DOIP_MAX_CONNECTIONS    (4U)
#endif

/**
 * [EDS#400] Per-read timeout in ms once a frame has STARTED — how long the
 * rest of a partially received header or payload may stall before the
 * connection is closed. This bounds a stall inside one frame only. It is
 * not P2Server (which bounds the ECU's own response time) and it is not
 * how long a tester may stay silent between requests — see
 * DOIP_IDLE_POLL_MS and the inactivity bounds below for that.
 */
#ifndef DOIP_TCP_RECV_TIMEOUT_MS
#define DOIP_TCP_RECV_TIMEOUT_MS (150U)
#endif

/*
 * [EDS#400] Inactivity between frames.
 *
 * Waiting for the first byte of the next frame is a different job from
 * finishing a frame already started, and gets its own timeout: each wait
 * is one tcp_recv() of DOIP_IDLE_POLL_MS, and the connection is closed
 * after a COUNT of consecutive waits that return DOIP_TCP_RECV_TIMEOUT.
 * Idleness needs no clock: a recv that timed out has already spent its
 * full timeout, so N timed-out polls are N x DOIP_IDLE_POLL_MS of
 * silence, as accurate as the kernel's own timeouts. (Measured on a
 * NUCLEO-H753ZI: 300 polls closed the connection after 298.96 s of host
 * wall time.)
 *
 * TRADE-OFF — the main risk of these values. The server holds ONE
 * connection at a time (DOIP_MAX_CONNECTIONS is a listen() backlog). Any
 * tolerance long enough for a real tester's ~2 s TesterPresent keep-alive
 * also lets a quiet tester, or one whose link died without a FIN/RST,
 * hold that slot while other testers wait in the backlog. Before EDS#400
 * that hold was ~150 ms, which also made it impossible to keep a session
 * or SecurityAccess unlock across any normal pause. Mitigations:
 *   - before routing activation the bound is ISO 13400-2's 2 s
 *     T_TCP_Initial_Inactivity, so a client that connects and never
 *     activates routing is still released quickly;
 *   - after it, ISO's 5 min T_TCP_General_Inactivity default;
 *   - total idleness per connection is capped
 *     (DOIP_MAX_CONNECTION_IDLE_POLLS), and so is total connection
 *     lifetime (DOIP_MAX_CONNECTION_LIFETIME_MS, EDS#412), so neither a
 *     client sending one frame just inside every inactivity window nor
 *     one sending frames continuously holds the slot indefinitely;
 *   - a peer that closes or resets its socket is dropped at once, since
 *     only DOIP_TCP_RECV_TIMEOUT counts as idleness.
 * A deployment where a second tester must never wait minutes should lower
 * DOIP_GENERAL_INACTIVITY_POLLS. Releasing a dead connection the moment
 * another tester arrives (ISO's alive check on socket contention) would
 * need the serial server to accept a second socket and is not implemented.
 */

/** [EDS#400] Per-read timeout in ms while waiting for a new frame. Also the
 * longest gap between supervision check-ins while connected and idle —
 * see eds_doip_set_checkin_callback(). */
#ifndef DOIP_IDLE_POLL_MS
#define DOIP_IDLE_POLL_MS (1000U)
#endif

/** [EDS#400] Idle polls allowed from accept until routing activation —
 * ISO 13400-2 T_TCP_Initial_Inactivity (2 s) at the default poll. Counted
 * from accept and not reset by frames that do not activate routing. */
#ifndef DOIP_INITIAL_INACTIVITY_POLLS
#define DOIP_INITIAL_INACTIVITY_POLLS (2U)
#endif

/** [EDS#400] Consecutive idle polls allowed between frames once routing is
 * active — ISO 13400-2 T_TCP_General_Inactivity (5 min) at the default
 * poll. */
#ifndef DOIP_GENERAL_INACTIVITY_POLLS
#define DOIP_GENERAL_INACTIVITY_POLLS (300U)
#endif

/** [EDS#400] Total idle polls allowed over one connection's lifetime (30 min
 * at the default poll), whether or not frames arrive in between. Counts
 * only idle time: a client that sends a frame within every poll never
 * accumulates any, which is what DOIP_MAX_CONNECTION_LIFETIME_MS bounds. */
#ifndef DOIP_MAX_CONNECTION_IDLE_POLLS
#define DOIP_MAX_CONNECTION_IDLE_POLLS (1800U)
#endif

/**
 * [SEC-DOIP-SEND-01] Send timeout in ms that both platform bindings set
 * (SO_SNDTIMEO) on every accepted connection. Without it a blocking
 * tcp_send() against a peer that stops reading waits for window space
 * forever, and the server never serves another tester. On expiry
 * tcp_send() fails, doip_send_all() gives up at once, and the connection
 * is closed. Bounds one dispatch's response send for
 * eds_doip_set_checkin_callback() supervision too.
 */
#ifndef DOIP_TCP_SEND_TIMEOUT_MS
#define DOIP_TCP_SEND_TIMEOUT_MS (2000U)
#endif

#if (DOIP_IDLE_POLL_MS == 0U) || (DOIP_INITIAL_INACTIVITY_POLLS == 0U) || \
    (DOIP_GENERAL_INACTIVITY_POLLS == 0U) || (DOIP_MAX_CONNECTION_IDLE_POLLS == 0U) || \
    (DOIP_TCP_SEND_TIMEOUT_MS == 0U)
#error "DoIP inactivity and send-timeout bounds must be non-zero (EDS#400)"
#endif

/**
 * [EDS#193] Maximum partial tcp_recv() attempts allowed while assembling
 * one header or one payload. DOIP_TCP_RECV_TIMEOUT_MS bounds each
 * individual read's *silence*, but a client sending 1 byte per window
 * makes "progress" on every read and never trips that timeout — this
 * bounds the total attempts instead, so frame assembly always terminates
 * even against a client trickling data indefinitely. Generous enough
 * that no legitimate fragmentation pattern should hit it (256 reads at a
 * 1-byte minimum still covers the full DOIP_MAX_PDU_SIZE payload with
 * margin); tune down if a tighter bound is wanted for a given deployment.
 */
#ifndef DOIP_MAX_FRAME_READ_ATTEMPTS
#define DOIP_MAX_FRAME_READ_ATTEMPTS (256U)
#endif

/** Poll timeout for each eds_doip_server_run() tcp_accept() call while no
 * client is connected — how often the accept loop wakes up to re-check. */
#ifndef DOIP_ACCEPT_POLL_TIMEOUT_MS
#define DOIP_ACCEPT_POLL_TIMEOUT_MS (5000U)
#endif

/**
 * [EDS#412] Total lifetime of one TCP connection in ms (default 30 min),
 * measured from accept with the uptime_ms platform op. When it is spent the
 * server closes the connection, which also runs the EDS#191 session and
 * security reset, so the single serial slot (DOIP_MAX_CONNECTIONS is a
 * listen() backlog, not a concurrency limit) passes to the next tester.
 *
 * This is the EDS#218 bound against a client holding the slot forever with
 * valid traffic. The idle limits above cannot provide it: a client sending a
 * frame every 0.9 s is never idle. Removing this constant removes that
 * protection.
 *
 * ASSUMPTION, KNOWN TO BE FALSE IN PART: 30 minutes bounds total connection
 * lifetime, which assumes no legitimate session exceeds it. Long workshop
 * sessions do, and are cut. It exists only until EDS#409 makes eviction
 * demand-driven (alive check when a second tester arrives), which can tell
 * a long session from a squatter. A firmware download is not at risk: 2 MB
 * transfers in well under a minute.
 *
 * Replaces DOIP_MAX_CONNECTION_FRAMES (500), which counted frames as a
 * stand-in for this deadline and cut every download above ~0.5 MB
 * (EDS#412).
 *
 * Debugger halts: the platform clock keeps running while the core is
 * halted, so a halt longer than this drops the tester on resume. Harmless;
 * the tester reconnects.
 */
#ifndef DOIP_MAX_CONNECTION_LIFETIME_MS
#define DOIP_MAX_CONNECTION_LIFETIME_MS (1800000U)
#endif

#if (DOIP_MAX_CONNECTION_LIFETIME_MS == 0U)
#error "DOIP_MAX_CONNECTION_LIFETIME_MS must be non-zero (EDS#412)"
#endif

/*
 * [EDS#392] Vehicle discovery bounds. Discovery is served on the server
 * thread between TCP waits, so these keep a UDP flood from starving the TCP
 * tester or the supervision check-in, and cap what the ECU sends to
 * unauthenticated peers: a reply is ~5x its request, so without a rate cap
 * the ECU is a reflector against a spoofed source.
 */

/** Datagrams read per wake before the server looks at TCP again. */
#ifndef DOIP_UDP_MAX_PER_WAKE
#define DOIP_UDP_MAX_PER_WAKE (4U)
#endif

/** Replies (responses and generic NACKs) sent per second; datagrams beyond
 * this are read and dropped, counted in udp_dropped. Announcements are not
 * counted against it. */
#ifndef DOIP_UDP_MAX_REPLIES_PER_S
#define DOIP_UDP_MAX_REPLIES_PER_S (16U)
#endif

/** ISO 13400-2 A_DoIP_Announce_Num: announcements sent per trigger. */
#ifndef DOIP_ANNOUNCE_NUM
#define DOIP_ANNOUNCE_NUM (3U)
#endif

/** ISO 13400-2 A_DoIP_Announce_Interval, ms between announcements. */
#ifndef DOIP_ANNOUNCE_INTERVAL_MS
#define DOIP_ANNOUNCE_INTERVAL_MS (500U)
#endif

/** ISO 13400-2 A_DoIP_Announce_Wait: the first announcement waits a random
 * 0..this many ms, so entities powered together do not collide. */
#ifndef DOIP_ANNOUNCE_WAIT_MAX_MS
#define DOIP_ANNOUNCE_WAIT_MAX_MS (500U)
#endif

/** UDP buffers. rx: the largest request served is 8 + 17 bytes; longer
 * datagrams are truncated and NACKed by their header. tx: the largest reply
 * is a 33-byte vehicle announcement plus the header. Not tuneable. */
#define DOIP_UDP_RX_BUF_LEN (64U)
#define DOIP_UDP_TX_BUF_LEN (48U)

#if (DOIP_ANNOUNCE_NUM > 255U)
#error "DOIP_ANNOUNCE_NUM must fit in a uint8_t (EDS#392)"
#endif

#if (DOIP_UDP_MAX_PER_WAKE == 0U) || (DOIP_UDP_MAX_REPLIES_PER_S == 0U) || \
    (DOIP_ANNOUNCE_INTERVAL_MS == 0U)
#error "DoIP discovery bounds must be non-zero (EDS#392)"
#endif

#ifdef DOIP_MAX_CONNECTION_FRAMES
#error "DOIP_MAX_CONNECTION_FRAMES was removed (EDS#412): set DOIP_MAX_CONNECTION_LIFETIME_MS instead"
#endif

/**
 * [EDS#218] Consecutive connections that dispatch zero diagnostic frames
 * (connect, then close/error/timeout before a single DiagnosticMessage is
 * handled — e.g. malformed header, no routing activation, or an
 * immediate disconnect) before the server treats accept-cycling as
 * abusive and starts rejecting new connections outright. Resets to 0 the
 * moment any connection dispatches at least one frame. Mitigates
 * repeated connect/disconnect cycling used to keep re-acquiring the
 * single serial connection slot without ever doing real work on it.
 */
#ifndef DOIP_RAPID_RECONNECT_THRESHOLD
#define DOIP_RAPID_RECONNECT_THRESHOLD (8U)
#endif

/**
 * [EDS#218] Number of subsequent connection attempts rejected outright
 * (accepted, then immediately closed with no processing) once
 * DOIP_RAPID_RECONNECT_THRESHOLD is hit. Counts down per rejected
 * attempt rather than a wall-clock cooldown: written when this file had
 * no clock. The uptime_ms op now provides one; converting this count is
 * left out of EDS#412 to keep that change small.
 */
#ifndef DOIP_RECONNECT_BACKOFF_REJECTS
#define DOIP_RECONNECT_BACKOFF_REJECTS (4U)
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
 *               DOIP_TCP_RECV_TIMEOUT if no data arrived within timeout_ms,
 *               any other negative value on error.
 *   tcp_close / tcp_server_close : void, best-effort.
 *
 * All pointer arguments are non-NULL unless otherwise noted.
 * ========================================================================== */

/**
 * [EDS#400] tcp_recv() return value for "no data within timeout_ms, socket
 * still open". Only this value counts as an idle client; any other
 * negative return means the socket is dead and closes the connection at
 * once. Chosen outside every errno range, so a binding's -errno can never
 * alias it. A binding that reports timeouts as some other negative value
 * still works: its connections close on the first quiet DOIP_IDLE_POLL_MS
 * wait, as they did after DOIP_TCP_RECV_TIMEOUT_MS before EDS#400.
 */
#define DOIP_TCP_RECV_TIMEOUT   (-1000)

/** [EDS#392] udp_recvfrom() return value for "no datagram queued". Outside
 * every errno range, like DOIP_TCP_RECV_TIMEOUT. */
#define DOIP_UDP_NO_DATA        (-1001)

/** [EDS#392] wait_readable() result bits. */
#define DOIP_READY_TCP          (0x01U)
#define DOIP_READY_UDP          (0x02U)

/** [EDS#392] IPv4 UDP peer, both fields in host byte order. */
typedef struct eds_doip_udp_peer {
    uint32_t addr;   /**< IPv4 address, e.g. 0xC0000201 for 192.0.2.1. */
    uint16_t port;   /**< UDP port. */
} eds_doip_udp_peer_t;

/** [EDS#392] Limited broadcast, where announcements go (port DOIP_PORT). */
#define DOIP_UDP_BROADCAST_ADDR (0xFFFFFFFFU)

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
     * May transmit fewer bytes than requested (a short write) — the caller
     * (doip_send_all() in doip_server.c) retries until the full buffer is
     * sent. That retry loop assumes BLOCKING-socket semantics: each call
     * either transmits >0 bytes, blocks until it can, or fails — it does
     * not busy-return 0 to mean "nothing sent, try again later" the way a
     * non-blocking socket's EWOULDBLOCK might. A non-blocking implementation
     * of this callback is not supported as-is.
     *
     * @param[in] conn_ctx  Connection handle from tcp_accept().
     * @param[in] data      Bytes to transmit.
     * @param[in] len       Number of bytes.
     * [SEC-DOIP-SEND-01] Must not block longer than DOIP_TCP_SEND_TIMEOUT_MS
     * waiting for the peer: set the socket's send timeout at accept. A
     * negative return is final; doip_send_all() does not retry it.
     *
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
     * @return Bytes received (>0), 0 if connection closed,
     *         DOIP_TCP_RECV_TIMEOUT if nothing arrived within timeout_ms,
     *         any other negative value on error.
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

    /**
     * @brief Current uptime in milliseconds.  [EDS#412]
     *
     * Monotonic, wrapping at UINT32_MAX. Measures connection lifetime
     * (DOIP_MAX_CONNECTION_LIFETIME_MS). Both in-tree bindings return
     * eds_platform_uptime_ms(). Required: a binding without it fails
     * eds_doip_register_platform(), because a clock stuck at 0 would give
     * a lifetime bound that never fires.
     *
     * @return Uptime in ms.
     */
    uint32_t (*uptime_ms)(void);

    /**
     * @brief Perform an ECUReset that a 0x11 request left pending.  [EDS#434]
     *
     * The server calls this after the 0x11 positive response has been
     * handed to tcp_send(), or right after dispatch when the response was
     * suppressed (0x11 with bit 7 set). service_0x11 only records the reset
     * (see uds_server_take_pending_reset()), so without this op ECUReset
     * over DoIP answered 51 xx and the ECU kept running.
     *
     * tcp_send() returning means the stack has the bytes, not the peer, so
     * the op must let them drain before resetting. Both in-tree bindings
     * call eds_platform_nvm_flush(), wait DOIP_RESET_TX_GRACE_MS, then call
     * eds_platform_ecu_reset(), the same sequence the CAN loops use.
     * Required: a binding without it fails eds_doip_register_platform(),
     * because a missing op would make ECUReset lie.
     *
     * Returns only for a reset type the platform does not support; the
     * server then carries on serving the connection.
     *
     * @param[in] reset_type  ECUReset sub-function, bit 7 cleared (0x01-0x7F).
     */
    void (*ecu_reset)(uint8_t reset_type);

    /**
     * @brief Sleep for about ms milliseconds.  [EDS#282]
     *
     * Optional. While a response-pending job has nothing to send, the
     * server waits with this between uds_server_poll() calls, so the tick
     * task gets the lock. NULL: the server polls again at once.
     *
     * @param[in] ms  Time to sleep.
     */
    void (*sleep_ms)(uint32_t ms);

    /*
     * [EDS#392] Vehicle discovery over UDP -- optional, all or none.
     * udp_open, udp_recvfrom, udp_sendto, udp_close and wait_readable are
     * either all set or all NULL (eds_doip_register_platform() refuses a
     * partial set). All NULL: no UDP socket, and eds_doip_server_run() makes
     * exactly the calls it made before EDS#392. get_eid is optional on its
     * own.
     */

    /**
     * @brief Open a UDP socket bound to INADDR_ANY:port, able to receive and
     *        send limited broadcasts (SO_BROADCAST).
     * @param[in]  port     UDP port (DOIP_PORT).
     * @param[out] udp_ctx  Opaque handle.
     * @return 0 on success, negative on failure. A failure leaves the server
     *         TCP-only; it does not stop it.
     */
    int (*udp_open)(uint16_t port, void **udp_ctx);

    /**
     * @brief Read one queued datagram without blocking.
     * A datagram longer than buf_len is truncated to buf_len.
     * @param[in]  udp_ctx  Handle from udp_open().
     * @param[out] buf      Destination.
     * @param[in]  buf_len  Capacity.
     * @param[out] peer     Sender address and port.
     * @return Bytes copied (>= 0), DOIP_UDP_NO_DATA if nothing is queued,
     *         any other negative value on error.
     */
    int (*udp_recvfrom)(void *udp_ctx, uint8_t *buf, size_t buf_len,
                        eds_doip_udp_peer_t *peer);

    /**
     * @brief Send one datagram. Must not block for long: a full send buffer
     *        is a failure, not a wait.
     * @return Bytes sent, or negative on error.
     */
    int (*udp_sendto)(void *udp_ctx, const eds_doip_udp_peer_t *peer,
                      const uint8_t *data, size_t len);

    /** @brief Close the UDP socket. */
    void (*udp_close)(void *udp_ctx);

    /**
     * @brief Wait until a TCP handle or the UDP socket is readable.
     * tcp_ctx is the listening socket (readable = a connection is pending)
     * or a connection (readable = data or end of stream).
     * @return A mask of DOIP_READY_TCP / DOIP_READY_UDP, 0 if timeout_ms
     *         passed with neither ready, negative on error. On an error the
     *         server falls back to the plain TCP call for that wait.
     */
    int (*wait_readable)(void *tcp_ctx, void *udp_ctx, uint32_t timeout_ms);

    /**
     * @brief This entity's EID, normally the interface MAC address.
     * Optional: NULL, or a negative return, gives an all-zero EID unless
     * eds_doip_discovery_cfg_t sets one.
     * @return 0 on success.
     */
    int (*get_eid)(uint8_t eid[DOIP_EID_LEN]);
} eds_doip_platform_ops_t;

/**
 * [EDS#434] Time the in-tree bindings' ecu_reset op waits, after the 0x11
 * response was handed to tcp_send(), before resetting. ASSUMPTION: the TCP
 * stack puts one small frame on the wire within this time on a direct
 * link. Same 50 ms the CAN loops give ISO-TP.
 */
#ifndef DOIP_RESET_TX_GRACE_MS
#define DOIP_RESET_TX_GRACE_MS (50U)
#endif

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
    uint8_t  rx_buf[DOIP_MAX_PDU_SIZE]; /**< Static TCP receive buffer — no malloc. */
    uint8_t  tx_buf[DOIP_MAX_PDU_SIZE]; /**< Static TCP transmit buffer — no malloc. */
    uds_msg_buf_t uds_req;        /**< Static UDS request buffer — never on task stack. */
    uds_msg_buf_t uds_resp;       /**< Static UDS response buffer — never on task stack. */
    uint32_t frames_received;    /**< Diagnostic counter: total frames received. */
    uint32_t frames_sent;        /**< Diagnostic counter: total frames sent. */
    /** [EDS#218] Resource-accounting counters — connections force-closed
     * for reaching DOIP_MAX_CONNECTION_LIFETIME_MS (EDS#412), and connection attempts
     * rejected outright during a DOIP_RECONNECT_BACKOFF_REJECTS window.
     * Diagnostic/telemetry only; never reset except at server init. */
    uint32_t connections_lifetime_capped;
    uint32_t connections_rate_rejected;
    /* [EDS#392] Vehicle discovery. udp_ctx is NULL when discovery is off
     * or its socket failed to open. */
    void    *udp_ctx;
    uint8_t  udp_rx_buf[DOIP_UDP_RX_BUF_LEN]; /**< Static UDP receive buffer. */
    uint8_t  udp_tx_buf[DOIP_UDP_TX_BUF_LEN]; /**< Static UDP transmit buffer. */
    uint32_t udp_rate_window_ms;  /**< Start of the current 1 s reply window. */
    uint32_t udp_replies_in_window;
    uint8_t  announce_remaining;  /**< Announcements left in this burst. */
    uint32_t announce_anchor_ms;  /**< Time the next delay is measured from. */
    uint32_t announce_delay_ms;   /**< Delay from the anchor to the next one. */
    /** [EDS#392] Diagnostic counters, never reset except at init. */
    uint32_t udp_open_failed;     /**< udp_open() failures (server ran TCP-only). */
    uint32_t udp_received;        /**< Datagrams read. */
    uint32_t udp_replied;         /**< Responses and NACKs sent. */
    uint32_t udp_dropped;         /**< Read and dropped by the reply rate cap. */
    uint32_t udp_send_failed;     /**< udp_sendto() failures, announcements included. */
    uint32_t announcements_sent;
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

/* --------------------------------------------------------------------------
 * [EDS#353] Lock callbacks -- optional in development/CI builds,
 * [EDS#358] required in production builds
 * -------------------------------------------------------------------------- */

/**
 * @brief Lock/unlock callback signatures around eds_doip_server_run()'s
 *        uds_server_process_request() call.
 *
 * This module calls into the UDS core on the caller's behalf but has no
 * visibility into what else touches the same uds_server_ctx_t. A DoIP-only
 * build still runs a second thread (its own UDS tick task — see EDS#191)
 * that calls uds_server_tick_1ms()/uds_periodic_tick_1ms() against the
 * same context; without a shared lock, that thread and this one race on
 * session/security state with no synchronization at all (EDS#353).
 */
typedef void (*eds_doip_lock_cb_t)(void);
typedef void (*eds_doip_unlock_cb_t)(void);

/**
 * @brief Register lock/unlock callbacks around the UDS dispatch call.
 *
 * Optional in development/CI builds; not registering reproduces today's
 * behaviour there. [EDS#358] In a production build
 * (EDS_BUILD_IS_PRODUCTION), eds_doip_server_run() refuses to run
 * (UDS_STATUS_ERR_NOT_INITIALIZED) unless both callbacks are registered
 * non-NULL -- correct only if nothing else ever calls into the same
 * uds_server_ctx_t from another thread, which is false the moment
 * EDS#191's UDS tick task exists (true of every shipped DoIP example).
 * Register the *same* lock/unlock pair your tick task already wraps
 * uds_server_tick_1ms()/uds_periodic_tick_1ms() with, so both call paths
 * serialize against each other.
 *
 * Call once, before eds_doip_server_run().
 *
 * @param[in] lock_cb    Called before uds_server_process_request(). NULL clears.
 * @param[in] unlock_cb  Called after uds_server_process_request(). NULL clears.
 */
void eds_doip_set_lock_callbacks(
    eds_doip_lock_cb_t   lock_cb,
    eds_doip_unlock_cb_t unlock_cb
);

/* --------------------------------------------------------------------------
 * [EDS#399] Supervision check-in -- optional
 * -------------------------------------------------------------------------- */

/**
 * @brief Check-in callback signature, called from the DoIP server thread.
 *
 * Runs on the server thread between platform calls, so it must not block.
 * Typical use: record "DoIP alive" (a counter or timestamp) that the
 * thread feeding the watchdog checks before it feeds, or feed a per-thread
 * channel such as Zephyr's task watchdog.
 */
typedef void (*eds_doip_checkin_cb_t)(void);

/**
 * @brief Register a callback the server loop calls each time it wakes.
 *
 * A watchdog fed from another thread does not supervise
 * eds_doip_server_run(): wedged in a socket call, or returned on a fatal
 * error, that thread stops while the feeding thread carries on. This
 * callback lets an integrator supervise the loop itself.
 *
 * Called after every tcp_accept() return (each accept poll, with or without
 * a client), every tcp_recv() return, and every frame dispatch. In normal
 * operation, idle or busy, the gap between two calls is therefore at most
 * DOIP_CHECKIN_MAX_WAIT_MS plus the time of one frame dispatch, including
 * its response tcp_send() (at most DOIP_TCP_SEND_TIMEOUT_MS per send in
 * the shipped bindings). With no client connected the server is still
 * healthy and still checks in on every DOIP_ACCEPT_POLL_TIMEOUT_MS wake.
 * Size the supervision window above DOIP_CHECKIN_MAX_WAIT_MS with margin.
 *
 * Gaps the window will report, by design: a tcp_accept()/tcp_recv() that
 * overruns its timeout, a dispatch blocked on the EDS#353 locks, and
 * eds_doip_server_run() having returned. A peer that stops reading its
 * socket blocks a response tcp_send() for at most DOIP_TCP_SEND_TIMEOUT_MS
 * in both shipped bindings, then the connection is closed; a custom
 * binding without a send timeout can block indefinitely there, which
 * with this callback wired to a watchdog becomes a reset.
 *
 * Not registering one leaves the server unchanged.
 *
 * Call before eds_doip_server_run(), as with eds_doip_set_lock_callbacks().
 *
 * @param[in] cb  Check-in callback. NULL clears it.
 */
void eds_doip_set_checkin_callback(eds_doip_checkin_cb_t cb);

/* --------------------------------------------------------------------------
 * [EDS#423] Response-pending continuation -- optional
 * -------------------------------------------------------------------------- */

/**
 * [EDS#423] Longest the server keeps one request's response-pending phase
 * going (see eds_doip_set_pending_callback()), in ms. ASSUMPTION: no pending
 * job needs longer. The one shipped user is RequestDownload's chunked erase,
 * at most 4 s per 128 KB step (the STM32H7 devicetree max-erase-time), so a
 * 2 MB region needs at most 64 s. A larger region on slow flash breaks this:
 * raise it, or the server closes the connection mid-erase.
 */
#ifndef DOIP_MAX_PENDING_MS
#define DOIP_MAX_PENDING_MS (120000U)
#endif

#if (DOIP_MAX_PENDING_MS == 0U)
#error "DOIP_MAX_PENDING_MS must be non-zero (EDS#423)"
#endif

/**
 * @brief Produce the next frame of a response-pending (NRC 0x78) exchange.
 *
 * @deprecated [EDS#282] The server now drives uds_server_poll() itself;
 * this callback type and eds_doip_set_pending_callback() go in the next
 * minor release.
 *
 * @param[out] out_frame  The frame to send: another 7F xx 78, or the final
 *                        response.
 * @return true if out_frame holds a frame to send, false if nothing is pending.
 */
typedef bool (*eds_doip_pending_cb_t)(uds_msg_buf_t *out_frame);

/**
 * @brief Register the callback that completes response-pending requests.
 *
 * @deprecated [EDS#282] Not needed any more: register nothing, and after a
 * 7F xx 78 response the server calls uds_server_poll() inside the
 * eds_doip_set_lock_callbacks() pair, sends each frame it returns,
 * reports it with uds_server_frame_sent(), and stops after the final
 * response. While the job has nothing to send it waits with the sleep_ms
 * platform op. With a callback registered, the server behaves as below,
 * as before EDS#282.
 *
 * Some services answer NRC 0x78 (requestCorrectlyReceived-ResponsePending)
 * and finish the job afterwards, one bounded step at a time. RequestDownload's
 * chunked erase does this when the flash ops provide erase_step_cb
 * (uds_service_0x34_pending_tick(), platform/zephyr/zephyr_flash_ops.c).
 *
 * With a callback registered, after the server sends a 7F xx 78 response it
 * calls cb inside the eds_doip_set_lock_callbacks() pair, sends the frame cb
 * returns, and repeats while cb keeps returning 7F xx 78. It stops at the
 * first other frame (the final response, sent), when cb returns false, or
 * after DOIP_MAX_PENDING_MS. In the last case the connection is closed.
 * The server checks in (eds_doip_set_checkin_callback()) before each call,
 * so the supervision gap during this phase is one cb call: for a chunked
 * erase, one sector erase (up to 4 s on STM32H7).
 *
 * Either way the server does not read from the connection meanwhile;
 * ISO 14229-1 has the tester wait for the final response. A disconnect
 * aborts the job (uds_server_pending_abort()).
 *
 * Call before eds_doip_server_run().
 *
 * @param[in] cb  Continuation callback, e.g. uds_service_0x34_pending_tick.
 *                NULL clears it.
 */
void eds_doip_set_pending_callback(eds_doip_pending_cb_t cb);

/* --------------------------------------------------------------------------
 * [EDS#392] Vehicle discovery -- on by default where the platform has UDP
 * -------------------------------------------------------------------------- */

/**
 * @brief Read the VIN for a vehicle identification response.
 * Called on the DoIP server thread inside the eds_doip_set_lock_callbacks()
 * pair, once per response, so a 0x2E write to the VIN DID shows at once.
 * Read it through the 0xF190 safety wrapper with uds_ctx's own session and
 * security contexts, never around them: if the DID is restricted (say, to
 * a non-default session) the wrapper refuses and the response reports the
 * VIN as not available.
 * @param[in]  uds_ctx  The UDS context passed to eds_doip_server_run().
 * @param[out] vin      17 bytes.
 * @return UDS_STATUS_OK with vin filled; anything else means not available.
 */
typedef uds_status_t (*eds_doip_vin_cb_t)(uds_server_ctx_t *uds_ctx,
                                          uint8_t vin[DOIP_VIN_LEN]);

/**
 * @brief Report the diagnostic power mode (DOIP_POWER_MODE_*). Called on the
 * server thread, must not block. Not called under the UDS lock.
 */
typedef uint8_t (*eds_doip_power_mode_cb_t)(void);

/** [EDS#392] Discovery configuration. Copied by eds_doip_set_discovery(). */
typedef struct eds_doip_discovery_cfg {
    bool     enabled;         /**< Answer discovery on UDP DOIP_PORT. */
    bool     announce;        /**< Announce at start and on eds_doip_announce_request(). */
    eds_doip_vin_cb_t get_vin; /**< NULL: VIN not available. */
    bool     eid_set;         /**< true: use eid below instead of the get_eid op. */
    uint8_t  eid[DOIP_EID_LEN];
    bool     gid_set;         /**< false: GID = EID. */
    uint8_t  gid[DOIP_GID_LEN];
    uint8_t  further_action;  /**< Normally DOIP_FURTHER_ACTION_NONE. */
    eds_doip_power_mode_cb_t get_power_mode; /**< NULL: always ready. */
} eds_doip_discovery_cfg_t;

/**
 * @brief Configure vehicle discovery (ISO 13400-2 0x0001-0x0004,
 *        0x4001-0x4004 over UDP).
 * Discovery runs whenever the platform ops include UDP. Without a call to
 * this function it runs with: enabled, announce, no VIN, EID from the
 * get_eid op, GID = EID, no further action, power mode ready.
 * Call before eds_doip_server_run(), like eds_doip_set_lock_callbacks().
 * @param[in] cfg  Copied. NULL restores the defaults above.
 */
void eds_doip_set_discovery(const eds_doip_discovery_cfg_t *cfg);

/**
 * @brief Ask the server to announce again (A_DoIP_Announce_Num times), for
 *        example when the interface gets an IPv4 address after a link flap.
 * Safe to call from another thread or a network event callback: it only
 * sets a flag the server thread consumes on its next wake, so the burst
 * starts up to DOIP_CHECKIN_MAX_WAIT_MS later (plus the A_DoIP_Announce_Wait
 * jitter). Ignored while announcements are off.
 */
void eds_doip_announce_request(void);

/** [EDS#399] Longest wait inside eds_doip_server_run() that EDS itself
 * bounds: the accept poll with no client, or one idle poll while
 * connected. See eds_doip_set_checkin_callback(). */
#define DOIP_CHECKIN_MAX_WAIT_MS \
    ((DOIP_ACCEPT_POLL_TIMEOUT_MS > DOIP_IDLE_POLL_MS) ? \
     DOIP_ACCEPT_POLL_TIMEOUT_MS : DOIP_IDLE_POLL_MS)

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
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if eds_doip_register_platform() not
 *         called, OR [EDS#358] if this is a production build
 *         (EDS_BUILD_IS_PRODUCTION) and eds_doip_set_lock_callbacks() was
 *         never called with both callbacks non-NULL -- see that function's
 *         doc comment. Development/CI builds are unaffected.
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

/**
 * @brief [EDS#392] Handle one datagram received on the discovery socket.
 * Called internally by eds_doip_server_run(). Exposed for unit testing.
 * Replies, if any, go to peer through the udp_sendto op, within the
 * DOIP_UDP_MAX_REPLIES_PER_S cap.
 * @param[in] s        DoIP server state, with its UDP socket open.
 * @param[in] uds_ctx  UDS server context (for the VIN read).
 * @param[in] buf      Datagram bytes.
 * @param[in] len      Datagram length.
 * @param[in] peer     Sender.
 * @return UDS_STATUS_OK if handled (answered, NACKed or ignored).
 * @return UDS_STATUS_ERR_NULL_PTR on a NULL argument, or no UDP socket.
 */
uds_status_t doip_handle_udp_datagram(doip_server_state_t       *s,
                                      uds_server_ctx_t          *uds_ctx,
                                      const uint8_t             *buf,
                                      uint32_t                   len,
                                      const eds_doip_udp_peer_t *peer);

#ifdef __cplusplus
}
#endif

#endif /* DOIP_SERVER_H */
