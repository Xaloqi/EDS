// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — native_sim self-test for examples/basic_ecu_doip
 * FILE: tests/zephyr_doip_selftest/doip_selftest.c
 *
 * PURPOSE: [EDS#197] An in-process DoIP client (loopback) drives one
 *   scenario against the example as built, prints its verdict and exits
 *   the native_sim process with it. Built only with
 *   -DDIAG_DOIP_SELFTEST=<mode> (examples/basic_ecu_doip/CMakeLists.txt),
 *   which also defines EDS_RACE_POINTS so EDS_RACE_POINT() reaches the hook
 *   below. Never in a firmware image.
 *
 *   SESSION_END: one S3 expiry and one disconnect, forced to meet. The
 *     tester opens 10 03 and goes silent. When S3 ends the session, the
 *     race point at the return-to-DEFAULT resets (EDS_RACE_SESSION_END)
 *     closes the tester's socket and sleeps 20 ms, so the DoIP thread
 *     handles the disconnect while the S3 reset is still in progress
 *     (k_msleep, not k_yield: the DoIP thread's priority is lower, and a
 *     yield never runs it). Expected: the application hears of one session
 *     end (one on_session_change), not two.
 *
 *   WDT_STALL: the EDS#399 supervision in the executor loop. The example
 *     runs 3 s with the watchdog armed and fed (no reset), then a call
 *     posted on the DoIP slot hangs the executor. Expected: the watchdog
 *     expires and resets; on native_sim the reset ends the process
 *     ("sys_arch_reboot called"). If the process is still alive 2 s after
 *     the stall, the test prints FAIL and exits 2.
 *
 * The addresses are the example's: tester 0x0E00, ECU 0xE400.
 * =============================================================================
 */

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/atomic.h>
#include <posix_board_if.h>   /* posix_exit(): native_sim only */

#include <stdint.h>
#include <string.h>

#include "uds_server.h"
#include "uds_session.h"
#include "uds_init.h"
#include "eds_race_point.h"
#if defined(DIAG_DOIP_SELFTEST_WDT_STALL)
#include "zephyr_exec.h"
#include "doip_server.h"   /* EDS_DOIP_UDS_SRC */
#endif

#define ST_TESTER        (0x0E00U)
#define ST_ECU           (0xE400U)
#define ST_PORT          (13400U)
#define ST_HDR           (8U)

#if defined(DIAG_DOIP_SELFTEST_SESSION_END)
static int             s_fd = -1;
static atomic_t        s_changes;
static atomic_t        s_armed;
static K_SEM_DEFINE(s_close_now, 0, 1);

static void st_count_change(uds_session_type_t old_s, uds_session_type_t new_s)
{
    (void)atomic_inc(&s_changes);
    printk("SELFTEST on_session_change(%d -> %d)\n", (int)old_s, (int)new_s);
}

/* EDS_RACE_SESSION_END, once armed: the tester disconnects now. */
static void st_race_hook(uint32_t id)
{
    if ((id == EDS_RACE_SESSION_END) && atomic_cas(&s_armed, 1, 0)) {
        printk("SELFTEST race point: S3 reset in progress, tester disconnects\n");
        k_sem_give(&s_close_now);
        k_msleep(20);   /* well inside the 100 ms watchdog window */
    }
}

static int st_send(uint16_t type, const uint8_t *p, uint32_t n)
{
    uint8_t b[ST_HDR + 16U];

    b[0] = 0x02U;
    b[1] = 0xFDU;
    b[2] = (uint8_t)(type >> 8);
    b[3] = (uint8_t)type;
    b[4] = (uint8_t)(n >> 24);
    b[5] = (uint8_t)(n >> 16);
    b[6] = (uint8_t)(n >> 8);
    b[7] = (uint8_t)n;
    memcpy(&b[ST_HDR], p, n);
    return (zsock_send(s_fd, b, ST_HDR + n, 0) == (ssize_t)(ST_HDR + n)) ? 0 : -1;
}

static int st_recv_exact(uint8_t *b, uint32_t n)
{
    uint32_t got = 0U;

    while (got < n) {
        ssize_t r = zsock_recv(s_fd, &b[got], n - got, 0);

        if (r <= 0) {
            return -1;
        }
        got += (uint32_t)r;
    }
    return 0;
}

/* One DoIP message; payload beyond cap is read and dropped. */
static int st_recv(uint16_t *type, uint8_t *p, uint32_t cap, uint32_t *n)
{
    uint8_t  h[ST_HDR];
    uint8_t  sink;
    uint32_t len;
    uint32_t i;

    if (st_recv_exact(h, ST_HDR) != 0) {
        return -1;
    }
    *type = (uint16_t)(((uint16_t)h[2] << 8) | h[3]);
    len   = ((uint32_t)h[4] << 24) | ((uint32_t)h[5] << 16) | ((uint32_t)h[6] << 8) | h[7];
    *n    = (len < cap) ? len : cap;
    if (st_recv_exact(p, *n) != 0) {
        return -1;
    }
    for (i = *n; i < len; i++) {
        if (st_recv_exact(&sink, 1U) != 0) {
            return -1;
        }
    }
    return 0;
}

static int st_connect(void)
{
    struct sockaddr_in a = { 0 };
    struct timeval     tv = { .tv_sec = 3, .tv_usec = 0 };
    int                tries;

    a.sin_family = AF_INET;
    a.sin_port   = htons(ST_PORT);
    (void)zsock_inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    for (tries = 0; tries < 20; tries++) {
        s_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if ((s_fd >= 0) && (zsock_connect(s_fd, (struct sockaddr *)&a, sizeof(a)) == 0)) {
            (void)zsock_setsockopt(s_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            return 0;
        }
        if (s_fd >= 0) {
            (void)zsock_close(s_fd);
        }
        k_msleep(500);
    }
    return -1;
}

/* Routing activation, then 10 03; 0 once 50 03 arrives. */
static int st_open_extended(void)
{
    static const uint8_t ra[]  = { (uint8_t)(ST_TESTER >> 8), (uint8_t)ST_TESTER, 0x00U,
                                   0x00U, 0x00U, 0x00U, 0x00U };
    static const uint8_t dsc[] = { (uint8_t)(ST_TESTER >> 8), (uint8_t)ST_TESTER,
                                   (uint8_t)(ST_ECU >> 8), (uint8_t)ST_ECU, 0x10U, 0x03U };
    uint8_t  p[32];
    uint16_t type;
    uint32_t n;
    int      i;

    if ((st_send(0x0005U, ra, sizeof(ra)) != 0) || (st_recv(&type, p, sizeof(p), &n) != 0) ||
        (type != 0x0006U) || (n < 5U) || (p[4] != 0x10U)) {
        printk("SELFTEST routing activation failed\n");
        return -1;
    }
    if (st_send(0x8001U, dsc, sizeof(dsc)) != 0) {
        return -1;
    }
    for (i = 0; i < 4; i++) {
        if (st_recv(&type, p, sizeof(p), &n) != 0) {
            break;
        }
        if ((type == 0x8001U) && (n >= 6U) && (p[4] == 0x50U) && (p[5] == 0x03U)) {
            return 0;
        }
    }
    printk("SELFTEST no 50 03\n");
    return -1;
}

static int st_session_end(void)
{
    uds_server_ctx_t *srv = uds_generated_get_server();
    long              n;

    if ((srv == NULL) || (st_connect() != 0) || (st_open_extended() != 0)) {
        printk("SELFTEST session_end: setup failed\n");
        return 1;
    }
    (void)uds_session_register_change_cb(srv->cfg.session_ctx, st_count_change);
    atomic_set(&s_changes, 0);
    atomic_set(&s_armed, 1);
    eds_race_point_hook = st_race_hook;
    printk("SELFTEST 10 03 open; waiting for S3\n");

    if (k_sem_take(&s_close_now, K_SECONDS(10)) != 0) {
        printk("SELFTEST session_end: S3 never ended the session\n");
        return 1;
    }
    (void)zsock_close(s_fd);
    k_msleep(1000);
    eds_race_point_hook = NULL;

    n = (long)atomic_get(&s_changes);
    printk("SELFTEST session_end: on_session_change calls=%ld expected=1 %s\n",
           n, (n == 1L) ? "PASS" : "FAIL");
    return (n == 1L) ? 0 : 1;
}

#endif /* DIAG_DOIP_SELFTEST_SESSION_END */

#if defined(DIAG_DOIP_SELFTEST_WDT_STALL)
/* Runs on the executor and never returns: the loop stalls. */
static void st_hang(uds_server_ctx_t *srv, void *arg)
{
    (void)srv;
    (void)arg;
    for (;;) {
        k_sleep(K_FOREVER);
    }
}

static int st_wdt_stall(void)
{
    k_msleep(3000);
    printk("SELFTEST wdt_stall: 3 s fed, no reset; stalling the executor\n");
    if (uds_exec_call(zephyr_exec_get(), (uint8_t)EDS_DOIP_UDS_SRC, st_hang, NULL) !=
        UDS_STATUS_OK) {
        printk("SELFTEST wdt_stall: could not post the call FAIL\n");
        return 2;
    }
    k_msleep(2000);
    printk("SELFTEST wdt_stall: still running 2 s after the stall FAIL\n");
    return 2;
}
#endif

static void st_entry(void *p1, void *p2, void *p3)
{
    int rc = 1;

    (void)p1;
    (void)p2;
    (void)p3;
#if defined(DIAG_DOIP_SELFTEST_SESSION_END)
    rc = st_session_end();
#elif defined(DIAG_DOIP_SELFTEST_WDT_STALL)
    rc = st_wdt_stall();
#endif
    printk("SELFTEST exit %d\n", rc);
    k_msleep(200);   /* let the deferred log drain */
    posix_exit(rc);
}

K_THREAD_DEFINE(doip_selftest, 4096, st_entry, NULL, NULL, NULL, 6, 0, 2000);
