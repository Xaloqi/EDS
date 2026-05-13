// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: transport/doip/freertos_lwip.c
 *
 * PURPOSE: FreeRTOS + LwIP platform binding for the DoIP TCP transport.
 *
 *          Implements eds_doip_platform_ops_t using LwIP BSD-socket API.
 *          All platform-specific code is isolated here; doip_server.c never
 *          calls lwip_* directly.
 *
 *          Socket lifecycle:
 *            freertos_tcp_listen() — lwip_socket + setsockopt + bind + listen
 *            freertos_tcp_accept() — lwip_select(timeout) + lwip_accept
 *            freertos_tcp_send()   — lwip_send(flags=0)
 *            freertos_tcp_recv()   — lwip_select(timeout) + lwip_recv
 *            freertos_tcp_close()  — lwip_close(conn_fd)
 *            freertos_tcp_server_close() — lwip_close(server_fd)
 *
 *          File descriptor representation:
 *            The eds_doip_platform_ops_t API uses void* for opaque connection
 *            and server contexts. The fd is stored as (void*)(uintptr_t)fd.
 *            This is the same pattern as zephyr_lwip.c (DEV-FD-01).
 *
 *          Task model:
 *            freertos_doip_platform_init() creates a dedicated FreeRTOS task
 *            (freertos_doip_task) that calls eds_doip_server_run(). The task
 *            blocks inside lwip_select() when idle, yielding to other tasks.
 *            Stack size and priority are configurable via freertos_doip_cfg_t.
 *
 *          LwIP integration requirements:
 *            - CONFIG_LWIP_TCP must be enabled
 *            - lwip_init() called and netif up before the DoIP task unblocks
 *            - LWIP_SO_RCVTIMEO=1 is NOT required — we use lwip_select()
 *
 * SAFETY  : Platform binding only. All safety logic in doip_server.c.
 *           ASIL-B candidate.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "freertos_lwip.h"
#include "doip_server.h"
#include "uds_server.h"
#include "uds_types.h"

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"

/* LwIP BSD socket API */
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/errno.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Module-level state (task parameters, one instance per application)
 * ------------------------------------------------------------------------ */

static doip_server_state_t s_doip_state;
static uds_server_ctx_t   *s_uds_ctx    = NULL;
static uint16_t             s_port       = DOIP_PORT;

/* ---------------------------------------------------------------------------
 * Platform ops — lwip_* implementations
 *
 * File descriptor as context: same DEV-FD-01 deviation as zephyr_lwip.c.
 * The fd is stored as (void*)(uintptr_t)(int32_t)fd. LwIP socket fds are
 * small non-negative integers; uintptr_t is wide enough on all targets.
 * ------------------------------------------------------------------------ */

static int freertos_tcp_listen(uint16_t port, void **server_ctx)
{
    int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }

    /* SO_REUSEADDR — allow rebind after ECU reset without TIME_WAIT delay */
    int opt = 1;
    (void)lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = lwip_htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (lwip_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        (void)lwip_close(fd);
        return -1;
    }

    if (lwip_listen(fd, (int)DOIP_MAX_CONNECTIONS) < 0) {
        (void)lwip_close(fd);
        return -1;
    }

    *server_ctx = (void *)(uintptr_t)(int32_t)fd;
    return 0;
}

static int freertos_tcp_accept(void *server_ctx, void **conn_ctx,
                                uint32_t timeout_ms)
{
    int server_fd = (int)(uintptr_t)server_ctx;

    /* Non-blocking accept via lwip_select with timeout */
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET((unsigned int)server_fd, &read_fds);

    struct timeval tv;
    tv.tv_sec  = (long)(timeout_ms / 1000U);
    tv.tv_usec = (long)((timeout_ms % 1000U) * 1000U);

    int rc = lwip_select(server_fd + 1, &read_fds, NULL, NULL, &tv);
    if (rc <= 0) {
        return -1; /* timeout or error */
    }

    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);
    int conn_fd = lwip_accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &addrlen);
    if (conn_fd < 0) {
        return -1;
    }

    *conn_ctx = (void *)(uintptr_t)(int32_t)conn_fd;
    return 0;
}

static int freertos_tcp_send(void *conn_ctx, const uint8_t *data, size_t len)
{
    int fd = (int)(uintptr_t)conn_ctx;
    int sent = lwip_send(fd, data, len, 0);
    if (sent < 0) {
        return -1;
    }
    return sent;
}

static int freertos_tcp_recv(void *conn_ctx, uint8_t *buf,
                              size_t buf_len, uint32_t timeout_ms)
{
    int fd = (int)(uintptr_t)conn_ctx;

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET((unsigned int)fd, &read_fds);

    struct timeval tv;
    tv.tv_sec  = (long)(timeout_ms / 1000U);
    tv.tv_usec = (long)((timeout_ms % 1000U) * 1000U);

    int rc = lwip_select(fd + 1, &read_fds, NULL, NULL, &tv);
    if (rc == 0) {
        return -1; /* timeout */
    }
    if (rc < 0) {
        return -1;
    }

    int bytes = lwip_recv(fd, buf, buf_len, 0);
    if (bytes < 0) {
        return -1;
    }
    /* bytes == 0: clean connection close */
    return bytes;
}

static void freertos_tcp_close(void *conn_ctx)
{
    int fd = (int)(uintptr_t)conn_ctx;
    (void)lwip_close(fd);
}

static void freertos_tcp_server_close(void *server_ctx)
{
    int fd = (int)(uintptr_t)server_ctx;
    (void)lwip_close(fd);
}

static const eds_doip_platform_ops_t s_freertos_doip_ops = {
    .tcp_listen        = freertos_tcp_listen,
    .tcp_accept        = freertos_tcp_accept,
    .tcp_send          = freertos_tcp_send,
    .tcp_recv          = freertos_tcp_recv,
    .tcp_close         = freertos_tcp_close,
    .tcp_server_close  = freertos_tcp_server_close,
};

/* ---------------------------------------------------------------------------
 * DoIP server task entry
 * ------------------------------------------------------------------------ */

static void freertos_doip_task(void *pvParameters)
{
    (void)pvParameters;

    /* Small yield to allow the network interface to come up fully */
    vTaskDelay(pdMS_TO_TICKS(500U));

    uds_status_t rc = eds_doip_server_run(&s_doip_state, s_uds_ctx, s_port);

    /* eds_doip_server_run() only returns on fatal error — suspend the task */
    (void)rc;
    vTaskSuspend(NULL);
}

/* ---------------------------------------------------------------------------
 * Public: freertos_doip_platform_init
 * ------------------------------------------------------------------------ */

uds_status_t freertos_doip_platform_init(const freertos_doip_cfg_t *cfg)
{
    if (cfg == NULL || cfg->uds_ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    uds_status_t rc = eds_doip_server_init(&s_doip_state, cfg->logical_address);
    if (rc != UDS_STATUS_OK) {
        return rc;
    }

    s_port    = cfg->port;
    s_uds_ctx = cfg->uds_ctx;

    rc = eds_doip_register_platform(&s_freertos_doip_ops);
    if (rc != UDS_STATUS_OK) {
        return rc;
    }

    /* Resolve stack size and priority */
    uint32_t    stack_words = (cfg->task_stack_size > 0U)
                              ? (cfg->task_stack_size / sizeof(StackType_t))
                              : (uint32_t)FREERTOS_DOIP_TASK_STACK_WORDS;
    UBaseType_t priority    = (cfg->task_priority > 0U)
                              ? cfg->task_priority
                              : (UBaseType_t)FREERTOS_DOIP_TASK_PRIORITY;

    BaseType_t created = xTaskCreate(
        freertos_doip_task,
        "doip_server",
        (configSTACK_DEPTH_TYPE)stack_words,
        NULL,
        priority,
        NULL
    );

    if (created != pdPASS) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    return UDS_STATUS_OK;
}
