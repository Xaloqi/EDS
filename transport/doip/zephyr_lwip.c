// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: transport/doip/zephyr_lwip.c
 *
 * PURPOSE: Zephyr platform binding for the DoIP TCP transport.
 *
 *          Implements eds_doip_platform_ops_t using the Zephyr BSD-socket
 *          API (zsock_*).  All platform-specific code is isolated here;
 *          doip_server.c never calls zsock_* directly.
 *
 *          Thread model:
 *            doip_thread — dedicated Zephyr thread defined via K_THREAD_DEFINE.
 *              Stack: CONFIG_DOIP_STACK_SIZE (default 4096 bytes).
 *              Priority: K_PRIO_PREEMPT(7) — lower than the ISO-TP diag_task
 *              (priority 5), so CAN diagnostics are never starved by DoIP.
 *
 *          Socket lifecycle:
 *            zephyr_tcp_listen() — zsock_socket + bind + listen
 *            zephyr_tcp_accept() — zsock_poll(timeout) + zsock_accept
 *            zephyr_tcp_send()   — zsock_send(flags=0)
 *            zephyr_tcp_recv()   — zsock_poll(timeout) + zsock_recv
 *            zephyr_tcp_close()  — zsock_close(conn_fd)
 *            zephyr_tcp_server_close() — zsock_close(server_fd)
 *          [EDS#392] With CONFIG_EDS_DOIP_DISCOVERY (default y): a UDP
 *          socket on the same port for vehicle discovery, polled together
 *          with the TCP socket (zephyr_wait_readable()); EID = the default
 *          interface's MAC; NET_EVENT_IPV4_ADDR_ADD re-announces.
 *
 *          SO_REUSEADDR is set on the server socket so native_sim CI
 *          restarts don't hit EADDRINUSE if the previous run did not
 *          fully drain the TIME_WAIT state.
 *
 * SAFETY  : Platform binding only. All safety logic lives in doip_server.c.
 *           ASIL-B candidate.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "zephyr_lwip.h"
#include "doip_server.h"
#include "uds_server.h"
#include "uds_types.h"
#include "platform_api.h"   /* [EDS#412] eds_platform_uptime_ms(); [EDS#434] reset */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_EDS_DOIP_DISCOVERY)
#include <zephyr/version.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(doip_zephyr, LOG_LEVEL_INF);

/* [SEC-DOIP-SEND-01] SO_SNDTIMEO is a silent no-op unless net_context
 * supports it. EDS's Kconfig defaults it on for every socket build;
 * refuse to build if a configuration has turned it off. */
#if !defined(CONFIG_NET_CONTEXT_SNDTIMEO)
#error "EDS DoIP needs CONFIG_NET_CONTEXT_SNDTIMEO=y: without a send timeout a tester that stops reading wedges the DoIP server (SEC-DOIP-SEND-01)."
#endif

/* [EDS#434] The ecu_reset op ends in sys_reboot(). EDS's Kconfig defaults
 * REBOOT on for every socket build; refuse to build if it was turned off,
 * rather than fail at link time or silently not reset. */
#if !defined(CONFIG_REBOOT)
#error "EDS DoIP needs CONFIG_REBOOT=y: ECUReset over DoIP is performed with sys_reboot() (EDS#434)."
#endif

/* ---------------------------------------------------------------------------
 * Kconfig-tunable thread parameters
 * ------------------------------------------------------------------------ */

#ifndef CONFIG_DOIP_STACK_SIZE
#define CONFIG_DOIP_STACK_SIZE   (4096U)
#endif

#ifndef CONFIG_DOIP_THREAD_PRIORITY
#define CONFIG_DOIP_THREAD_PRIORITY  (7)
#endif

/* ---------------------------------------------------------------------------
 * Module-level state
 *
 * Stored at module scope so the DoIP thread entry function can access them
 * without requiring a thread argument (K_THREAD_DEFINE takes no void* arg
 * per-instance — the entry function signature is fixed).
 * ------------------------------------------------------------------------ */

static doip_server_state_t s_doip_state;
static uds_server_ctx_t   *s_uds_ctx    = NULL;
static uint16_t             s_port       = DOIP_PORT;

/* [EDS#192] doip_thread blocks on this instead of assuming a fixed startup
 * delay. Given by zephyr_doip_platform_init() once s_uds_ctx is set and
 * platform ops are registered — a real synchronization point, not a
 * timing budget that a slower init path could silently exceed. */
static K_SEM_DEFINE(s_doip_ready_sem, 0, 1);

/* ---------------------------------------------------------------------------
 * Platform ops — zsock_* implementations
 * ------------------------------------------------------------------------ */

/*
 * File-descriptor wrapper.
 *
 * The eds_doip_platform_ops_t API passes conn/server contexts as void*.
 * On Zephyr with zsock_*, the context is simply the file descriptor cast to
 * void* via uintptr_t.  This is safe: Zephyr fds are small non-negative ints;
 * uintptr_t is wide enough on all supported architectures.
 */

static int zephyr_tcp_listen(uint16_t port, void **server_ctx)
{
    int fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        LOG_ERR("DoIP: zsock_socket failed: %d", errno);
        return -errno;
    }

    /* SO_REUSEADDR — allow rebind immediately after ECU reset */
    int opt = 1;
    (void)zsock_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (zsock_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("DoIP: zsock_bind port %u failed: %d", (unsigned)port, errno);
        (void)zsock_close(fd);
        return -errno;
    }

    if (zsock_listen(fd, (int)DOIP_MAX_CONNECTIONS) < 0) {
        LOG_ERR("DoIP: zsock_listen failed: %d", errno);
        (void)zsock_close(fd);
        return -errno;
    }

    LOG_INF("DoIP: listening on port %u", (unsigned)port);
    *server_ctx = (void *)(uintptr_t)(uint32_t)fd;
    return 0;
}

static int zephyr_tcp_accept(void *server_ctx, void **conn_ctx,
                              uint32_t timeout_ms)
{
    int server_fd = (int)(uintptr_t)server_ctx;

    /* Poll with timeout so the server loop can yield periodically */
    struct zsock_pollfd pfd = {
        .fd     = server_fd,
        .events = ZSOCK_POLLIN,
    };
    int rc = zsock_poll(&pfd, 1, (int)timeout_ms);
    if (rc <= 0) {
        /* 0 = timeout, <0 = error — both treated as "no connection yet" */
        return -EAGAIN;
    }

    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);
    int conn_fd = zsock_accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &addrlen);
    if (conn_fd < 0) {
        LOG_ERR("DoIP: zsock_accept failed: %d", errno);
        return -errno;
    }

    /* [SEC-DOIP-SEND-01] Bound every send on this connection. Without a
     * send timeout, zsock_send() waits K_FOREVER for window space, so a
     * peer that stops reading parks the server thread for good. Fail
     * closed: a connection that cannot get the bound is not served. */
    const uint32_t snd_ms = DOIP_TCP_SEND_TIMEOUT_MS;
    struct zsock_timeval snd_tv = {
        .tv_sec  = (long)snd_ms / 1000L,
        .tv_usec = ((long)snd_ms % 1000L) * 1000L,
    };
    if (zsock_setsockopt(conn_fd, SOL_SOCKET, SO_SNDTIMEO,
                         &snd_tv, sizeof(snd_tv)) < 0) {
        LOG_ERR("DoIP: SO_SNDTIMEO failed: %d; refusing connection", errno);
        (void)zsock_close(conn_fd);
        return -EIO;
    }

    /* [EDS#406] Disable Nagle. Each request is answered with two writes,
     * the 0x8002 ack and then the response; with Nagle the response waits
     * for the tester to ACK the first, which a Linux tester delays ~40 ms.
     * Needs no Kconfig (native TCP handles it unconditionally). A failure
     * only costs latency, so the connection is still served. */
    const int nodelay = 1;
    if (zsock_setsockopt(conn_fd, IPPROTO_TCP, TCP_NODELAY,
                         &nodelay, sizeof(nodelay)) < 0) {
        LOG_WRN("DoIP: TCP_NODELAY failed: %d; responses may be delayed", errno);
    }

    LOG_INF("DoIP: client connected (fd=%d)", conn_fd);
    *conn_ctx = (void *)(uintptr_t)(uint32_t)conn_fd;
    return 0;
}

static int zephyr_tcp_send(void *conn_ctx, const uint8_t *data, size_t len)
{
    int fd = (int)(uintptr_t)conn_ctx;
    ssize_t sent = zsock_send(fd, data, len, 0);
    if (sent < 0) {
        LOG_DBG("DoIP: zsock_send failed: %d", errno);
        return -errno;
    }
    return (int)sent;
}

static int zephyr_tcp_recv(void *conn_ctx, uint8_t *buf,
                            size_t buf_len, uint32_t timeout_ms)
{
    int fd = (int)(uintptr_t)conn_ctx;

    struct zsock_pollfd pfd = {
        .fd     = fd,
        .events = ZSOCK_POLLIN,
    };
    int rc = zsock_poll(&pfd, 1, (int)timeout_ms);
    if (rc == 0) {
        return DOIP_TCP_RECV_TIMEOUT; /* [EDS#400] quiet, not dead */
    }
    if (rc < 0) {
        return -errno;
    }

    ssize_t bytes = zsock_recv(fd, buf, buf_len, 0);
    if (bytes < 0) {
        LOG_DBG("DoIP: zsock_recv failed: %d", errno);
        return -errno;
    }
    /* bytes == 0 means clean connection close */
    return (int)bytes;
}

static void zephyr_tcp_close(void *conn_ctx)
{
    int fd = (int)(uintptr_t)conn_ctx;
    LOG_INF("DoIP: client disconnected (fd=%d)", fd);
    (void)zsock_close(fd);
}

static void zephyr_tcp_server_close(void *server_ctx)
{
    int fd = (int)(uintptr_t)server_ctx;
    (void)zsock_close(fd);
}

#if defined(CONFIG_EDS_DOIP_DISCOVERY)
/* ---------------------------------------------------------------------------
 * [EDS#392] Vehicle discovery: UDP socket on DOIP_PORT
 *
 * The UDP context is fd + 1, never the bare fd: the server reads a NULL
 * udp_ctx as "no discovery socket", and fd 0 cast to void* is NULL.
 * ------------------------------------------------------------------------ */

#if !defined(CONFIG_NET_UDP)
#error "CONFIG_EDS_DOIP_DISCOVERY needs CONFIG_NET_UDP=y (EDS#392)."
#endif

static int udp_fd(const void *udp_ctx)
{
    return (int)(uintptr_t)udp_ctx - 1;
}

static int zephyr_udp_open(uint16_t port, void **udp_ctx)
{
    int fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        LOG_ERR("DoIP: UDP zsock_socket failed: %d; discovery off", errno);
        return -errno;
    }

    int opt = 1;
    (void)zsock_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    /* Best effort: Zephyr sends to 255.255.255.255 without it on the
     * versions EDS supports, and a stack that needs it accepts it. */
    (void)zsock_setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (zsock_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("DoIP: UDP bind port %u failed: %d; discovery off", (unsigned)port, errno);
        (void)zsock_close(fd);
        return -errno;
    }

    LOG_INF("DoIP: vehicle discovery on UDP port %u", (unsigned)port);
    *udp_ctx = (void *)(uintptr_t)((uint32_t)fd + 1U);
    return 0;
}

static int zephyr_udp_recvfrom(void *udp_ctx, uint8_t *buf, size_t buf_len,
                               eds_doip_udp_peer_t *peer)
{
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    (void)memset(&from, 0, sizeof(from));
    ssize_t n = zsock_recvfrom(udp_fd(udp_ctx), buf, buf_len, ZSOCK_MSG_DONTWAIT,
                               (struct sockaddr *)&from, &fromlen);
    if (n < 0) {
        return ((errno == EAGAIN) || (errno == EWOULDBLOCK)) ? DOIP_UDP_NO_DATA : -errno;
    }
    peer->addr = ntohl(from.sin_addr.s_addr);
    peer->port = ntohs(from.sin_port);
    return (int)n;
}

static int zephyr_udp_sendto(void *udp_ctx, const eds_doip_udp_peer_t *peer,
                             const uint8_t *data, size_t len)
{
    struct sockaddr_in to;

    (void)memset(&to, 0, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_port        = htons(peer->port);
    to.sin_addr.s_addr = htonl(peer->addr);
    /* MSG_DONTWAIT: a full send buffer is a dropped datagram, not a wait
     * on the server thread. */
    ssize_t n = zsock_sendto(udp_fd(udp_ctx), data, len, ZSOCK_MSG_DONTWAIT,
                             (struct sockaddr *)&to, sizeof(to));
    if (n < 0) {
        LOG_DBG("DoIP: UDP sendto failed: %d", errno);
        return -errno;
    }
    return (int)n;
}

static void zephyr_udp_close(void *udp_ctx)
{
    (void)zsock_close(udp_fd(udp_ctx));
}

static int zephyr_wait_readable(void *tcp_ctx, void *udp_ctx, uint32_t timeout_ms)
{
    struct zsock_pollfd pfd[2] = {
        { .fd = (int)(uintptr_t)tcp_ctx, .events = ZSOCK_POLLIN },
        { .fd = udp_fd(udp_ctx),         .events = ZSOCK_POLLIN },
    };
    int rc = zsock_poll(pfd, 2, (int)timeout_ms);
    if (rc < 0) {
        return -errno;
    }
    uint32_t mask = 0U;
    /* POLLHUP/POLLERR on the connection count as readable: tcp_recv()
     * then reports the close or error, as before. */
    const uint32_t tcp_ready = (uint32_t)ZSOCK_POLLIN | (uint32_t)ZSOCK_POLLHUP |
                               (uint32_t)ZSOCK_POLLERR;
    if (((uint32_t)(uint16_t)pfd[0].revents & tcp_ready) != 0U) {
        mask |= DOIP_READY_TCP;
    }
    if (((uint32_t)(uint16_t)pfd[1].revents & (uint32_t)ZSOCK_POLLIN) != 0U) {
        mask |= DOIP_READY_UDP;
    }
    return (int)mask;
}

/* EID = the default interface's 6-byte link-layer (MAC) address. */
static int zephyr_get_eid(uint8_t eid[DOIP_EID_LEN])
{
    struct net_if *iface = net_if_get_default();
    if (iface == NULL) {
        return -ENODEV;
    }
    struct net_linkaddr *ll = net_if_get_link_addr(iface);
    if ((ll == NULL) || (ll->len != DOIP_EID_LEN)) {
        return -ENODEV;   /* loopback has no MAC: all-zero EID */
    }
    (void)memcpy(eid, ll->addr, DOIP_EID_LEN);
    return 0;
}

#if defined(CONFIG_NET_MGMT_EVENT)
/* Announce again when the interface gets an IPv4 address (DHCP, a static
 * address applied late, or a link that came back). */
static struct net_mgmt_event_callback s_ipv4_cb;

/* Zephyr 4.2 widened the event code from uint32_t to uint64_t. */
#if ZEPHYR_VERSION_CODE >= ZEPHYR_VERSION(4, 2, 0)
typedef uint64_t doip_mgmt_event_t;
#else
typedef uint32_t doip_mgmt_event_t;
#endif

static void doip_ipv4_event(struct net_mgmt_event_callback *cb,
                            doip_mgmt_event_t mgmt_event, struct net_if *iface)
{
    (void)cb; (void)iface;
    if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
        eds_doip_announce_request();
    }
}
#endif /* CONFIG_NET_MGMT_EVENT */
#endif /* CONFIG_EDS_DOIP_DISCOVERY */

/* [EDS#434] Same sequence as the CAN loops: persist, let the 0x11
 * response leave the stack, reset. Does not return for a supported type. */
static void zephyr_doip_ecu_reset(uint8_t reset_type)
{
    (void)eds_platform_nvm_flush();
    k_msleep((int32_t)DOIP_RESET_TX_GRACE_MS);
    (void)eds_platform_ecu_reset(reset_type);
}

/* [EDS#282] Wait between polls of a pending job. */
static void zephyr_doip_sleep_ms(uint32_t ms)
{
    k_msleep((int32_t)ms);
}

static const eds_doip_platform_ops_t s_zephyr_doip_ops = {
    .tcp_listen        = zephyr_tcp_listen,
    .tcp_accept        = zephyr_tcp_accept,
    .tcp_send          = zephyr_tcp_send,
    .tcp_recv          = zephyr_tcp_recv,
    .tcp_close         = zephyr_tcp_close,
    .tcp_server_close  = zephyr_tcp_server_close,
    .uptime_ms         = eds_platform_uptime_ms,  /* [EDS#412] */
    .ecu_reset         = zephyr_doip_ecu_reset,   /* [EDS#434] */
    .sleep_ms          = zephyr_doip_sleep_ms, /* [EDS#282] */
#if defined(CONFIG_EDS_DOIP_DISCOVERY)
    .udp_open          = zephyr_udp_open,      /* [EDS#392] */
    .udp_recvfrom      = zephyr_udp_recvfrom,
    .udp_sendto        = zephyr_udp_sendto,
    .udp_close         = zephyr_udp_close,
    .wait_readable     = zephyr_wait_readable,
    .get_eid           = zephyr_get_eid,
#endif
};

/* ---------------------------------------------------------------------------
 * DoIP server thread entry
 * ------------------------------------------------------------------------ */

static void doip_thread_entry(void *p1, void *p2, void *p3)
{
    (void)p1; (void)p2; (void)p3;

    /* [EDS#192] Block until zephyr_doip_platform_init() has set s_uds_ctx
     * and registered platform ops, rather than assuming a fixed delay is
     * always enough. A slower init path (more DIDs, a different NVM
     * backend, a debug build) no longer silently races this thread. */
    k_sem_take(&s_doip_ready_sem, K_FOREVER);

    LOG_INF("DoIP server thread starting (logical_addr=0x%04X port=%u)",
            (unsigned)s_doip_state.logical_address, (unsigned)s_port);

    uds_status_t rc = eds_doip_server_run(&s_doip_state, s_uds_ctx, s_port);

    /* eds_doip_server_run() only returns on fatal error */
    LOG_ERR("DoIP server exited unexpectedly: 0x%02X", (unsigned)rc);
}

/* Thread defined at link time — started by the Zephyr kernel once all
 * static initialisers have run. Its body immediately blocks on
 * s_doip_ready_sem (see above) until zephyr_doip_platform_init() signals
 * it — no artificial startup delay. */
K_THREAD_DEFINE(doip_thread,
                CONFIG_DOIP_STACK_SIZE,
                doip_thread_entry,
                NULL, NULL, NULL,
                K_PRIO_PREEMPT(CONFIG_DOIP_THREAD_PRIORITY),
                0,
                0);

/* ---------------------------------------------------------------------------
 * Public: zephyr_doip_platform_init
 * ------------------------------------------------------------------------ */

uds_status_t zephyr_doip_platform_init(uint16_t          logical_address,
                                        uint16_t          port,
                                        uds_server_ctx_t *uds_ctx)
{
    if (uds_ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    uds_status_t rc = eds_doip_server_init(&s_doip_state, logical_address);
    if (rc != UDS_STATUS_OK) {
        return rc;
    }

    s_port    = port;
    s_uds_ctx = uds_ctx;

    rc = eds_doip_register_platform(&s_zephyr_doip_ops);
#if defined(CONFIG_EDS_DOIP_DISCOVERY) && defined(CONFIG_NET_MGMT_EVENT)
    if (rc == UDS_STATUS_OK) {
        /* [EDS#392] Re-announce when an IPv4 address is added. */
        net_mgmt_init_event_callback(&s_ipv4_cb, doip_ipv4_event,
                                     NET_EVENT_IPV4_ADDR_ADD);
        net_mgmt_add_event_callback(&s_ipv4_cb);
    }
#endif
    if (rc == UDS_STATUS_OK) {
        /* [EDS#192] s_uds_ctx and platform ops are both ready — release
         * doip_thread now instead of leaving it to a fixed delay. */
        k_sem_give(&s_doip_ready_sem);
    }
    return rc;
}
