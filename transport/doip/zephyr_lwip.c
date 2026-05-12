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
 *            zephyr_tcp_send()   — zsock_send(MSG_NOSIGNAL)
 *            zephyr_tcp_recv()   — zsock_poll(timeout) + zsock_recv
 *            zephyr_tcp_close()  — zsock_close(conn_fd)
 *            zephyr_tcp_server_close() — zsock_close(server_fd)
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

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(doip_zephyr, LOG_LEVEL_INF);

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

    LOG_INF("DoIP: client connected (fd=%d)", conn_fd);
    *conn_ctx = (void *)(uintptr_t)(uint32_t)conn_fd;
    return 0;
}

static int zephyr_tcp_send(void *conn_ctx, const uint8_t *data, size_t len)
{
    int fd = (int)(uintptr_t)conn_ctx;
    ssize_t sent = zsock_send(fd, data, len, MSG_NOSIGNAL);
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
        return -EAGAIN; /* timeout */
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

static const eds_doip_platform_ops_t s_zephyr_doip_ops = {
    .tcp_listen        = zephyr_tcp_listen,
    .tcp_accept        = zephyr_tcp_accept,
    .tcp_send          = zephyr_tcp_send,
    .tcp_recv          = zephyr_tcp_recv,
    .tcp_close         = zephyr_tcp_close,
    .tcp_server_close  = zephyr_tcp_server_close,
};

/* ---------------------------------------------------------------------------
 * DoIP server thread entry
 * ------------------------------------------------------------------------ */

static void doip_thread_entry(void *p1, void *p2, void *p3)
{
    (void)p1; (void)p2; (void)p3;

    LOG_INF("DoIP server thread starting (logical_addr=0x%04X port=%u)",
            (unsigned)s_doip_state.logical_address, (unsigned)s_port);

    uds_status_t rc = eds_doip_server_run(&s_doip_state, s_uds_ctx, s_port);

    /* eds_doip_server_run() only returns on fatal error */
    LOG_ERR("DoIP server exited unexpectedly: 0x%02X", (unsigned)rc);
}

/* Thread defined at link time — started by the Zephyr kernel once
 * all static initialisers have run.  The thread body blocks in
 * eds_doip_server_run() → tcp_listen() until zephyr_doip_platform_init()
 * has been called and s_uds_ctx is non-NULL. */
K_THREAD_DEFINE(doip_thread,
                CONFIG_DOIP_STACK_SIZE,
                doip_thread_entry,
                NULL, NULL, NULL,
                K_PRIO_PREEMPT(CONFIG_DOIP_THREAD_PRIORITY),
                0,
                /* delay_ms = */ 500);  /* 500 ms head-start for UDS init */

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
    s_uds_ctx = uds_ctx;   /* thread reads this after 500 ms delay */

    return eds_doip_register_platform(&s_zephyr_doip_ops);
}
