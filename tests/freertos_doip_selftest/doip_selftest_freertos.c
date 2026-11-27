// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — QEMU self-test for examples/basic_ecu_doip_freertos
 * FILE: tests/freertos_doip_selftest/doip_selftest_freertos.c
 *
 * PURPOSE: [EDS#197] The FreeRTOS twin of tests/zephyr_doip_selftest
 *   (SESSION_END). An in-firmware DoIP client on lwIP's loopback interface
 *   opens 10 03 and goes silent. When S3 ends the session, the race point
 *   at the return-to-DEFAULT resets (EDS_RACE_SESSION_END) wakes the client,
 *   which closes its socket, and holds that task 20 ms, so the DoIP task
 *   handles the disconnect while the S3 reset is still in progress.
 *   Expected: one on_session_change, not two.
 *
 *   Then it prints the stack high-water mark of the executor (or the tick
 *   task it replaces) and the DoIP task, read after the workload, and exits
 *   QEMU through semihosting: 0 on PASS, 1 on FAIL (run QEMU with
 *   -semihosting).
 *
 *   Built only with -DDIAG_DOIP_SELFTEST=SESSION_END and a real lwIP
 *   (-DLWIP_DIR), which also defines EDS_RACE_POINTS (the race-point hook)
 *   and turns on lwIP's loopback interface. Never a firmware image.
 *
 * The addresses are the example's: tester 0x0E00, ECU 0xE400.
 * =============================================================================
 */

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "lwip/sockets.h"

#include <stdint.h>
#include <string.h>

#include "board_uart.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_init.h"
#include "eds_race_point.h"

#define ST_TESTER        (0x0E00U)
#define ST_ECU           (0xE400U)
#define ST_PORT          (13400U)
#define ST_HDR           (8U)
#define ST_STACK_WORDS   (1024U)
#define ST_PRIORITY      (4U)

static StaticTask_t      s_st_tcb;
static StackType_t       s_st_stack[ST_STACK_WORDS];
static StaticSemaphore_t s_close_buf;
static SemaphoreHandle_t s_close_now;
static int               s_fd = -1;
static volatile uint32_t s_changes;
static volatile uint32_t s_armed;

static void st_puts(const char *s)
{
    board_uart_puts(s);
}

static void st_count_change(uds_session_type_t old_s, uds_session_type_t new_s)
{
    s_changes++;
    st_puts("SELFTEST on_session_change(");
    board_uart_put_u32((uint32_t)old_s);
    st_puts(" -> ");
    board_uart_put_u32((uint32_t)new_s);
    st_puts(")\r\n");
}

/* EDS_RACE_SESSION_END, once armed: the tester disconnects now. */
static void st_race_hook(uint32_t id)
{
    if ((id == EDS_RACE_SESSION_END) && (s_armed != 0U)) {
        s_armed = 0U;
        st_puts("SELFTEST race point: S3 reset in progress, tester disconnects\r\n");
        (void)xSemaphoreGive(s_close_now);
        vTaskDelay(pdMS_TO_TICKS(20));
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
    return (lwip_send(s_fd, b, ST_HDR + n, 0) == (int)(ST_HDR + n)) ? 0 : -1;
}

static int st_recv_exact(uint8_t *b, uint32_t n)
{
    uint32_t got = 0U;

    while (got < n) {
        int r = lwip_recv(s_fd, &b[got], n - got, 0);

        if (r <= 0) {
            return -1;
        }
        got += (uint32_t)r;
    }
    return 0;
}

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
    struct sockaddr_in a;
    struct timeval     tv = { .tv_sec = 3, .tv_usec = 0 };
    int                tries;

    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = lwip_htons(ST_PORT);
    a.sin_addr.s_addr = lwip_htonl(0x7F000001UL);   /* 127.0.0.1, lwIP's loopif */
    for (tries = 0; tries < 20; tries++) {
        s_fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if ((s_fd >= 0) && (lwip_connect(s_fd, (struct sockaddr *)&a, sizeof(a)) == 0)) {
            (void)lwip_setsockopt(s_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            return 0;
        }
        if (s_fd >= 0) {
            (void)lwip_close(s_fd);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return -1;
}

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
        st_puts("SELFTEST routing activation failed\r\n");
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
    st_puts("SELFTEST no 50 03\r\n");
    return -1;
}

static void st_stack(const char *name)
{
    TaskHandle_t t = xTaskGetHandle(name);

    if (t != NULL) {
        st_puts("SELFTEST stack ");
        st_puts(name);
        st_puts(" free_bytes=");
        board_uart_put_u32((uint32_t)uxTaskGetStackHighWaterMark(t) * (uint32_t)sizeof(StackType_t));
        st_puts("\r\n");
    }
}

/* Semihosting SYS_EXIT: ApplicationExit (QEMU exits 0) or RunTimeErrorUnknown (1). */
static void st_exit(int pass)
{
    register uint32_t r0 __asm__("r0") = 0x18U;
    register uint32_t r1 __asm__("r1") = (pass != 0) ? 0x20026U : 0x20023U;

    __asm__ volatile ("bkpt 0xAB" : : "r"(r0), "r"(r1) : "memory");
    for (;;) {
    }
}

static void st_task(void *arg)
{
    uds_server_ctx_t *srv = uds_generated_get_server();
    int               pass = 0;

    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));   /* lwIP and the DoIP listener come up */
    if ((srv == NULL) || (st_connect() != 0) || (st_open_extended() != 0)) {
        st_puts("SELFTEST session_end: setup failed\r\n");
    } else {
        (void)uds_session_register_change_cb(srv->cfg.session_ctx, st_count_change);
        s_changes = 0U;
        s_armed   = 1U;
        eds_race_point_hook = st_race_hook;
        st_puts("SELFTEST 10 03 open; waiting for S3\r\n");
        if (xSemaphoreTake(s_close_now, pdMS_TO_TICKS(10000)) != pdTRUE) {
            st_puts("SELFTEST session_end: S3 never ended the session\r\n");
        } else {
            (void)lwip_close(s_fd);
            vTaskDelay(pdMS_TO_TICKS(1000));
            eds_race_point_hook = NULL;
            pass = (s_changes == 1U) ? 1 : 0;
            st_puts("SELFTEST session_end: on_session_change calls=");
            board_uart_put_u32(s_changes);
            st_puts((pass != 0) ? " expected=1 PASS\r\n" : " expected=1 FAIL\r\n");
        }
    }
    st_stack("eds_exec");
    st_stack("uds_tick");
    st_stack("doip_server");
    st_exit(pass);
}

/* Called by the example's main() before vTaskStartScheduler(). */
void doip_selftest_start(void)
{
    s_close_now = xSemaphoreCreateBinaryStatic(&s_close_buf);
    (void)xTaskCreateStatic(st_task, "selftest", ST_STACK_WORDS, NULL, ST_PRIORITY,
                            s_st_stack, &s_st_tcb);
}
