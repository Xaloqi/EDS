// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/basic_ecu_doip_freertos/src/main.c
 *
 * PURPOSE: BasicECU_DoIP_FreeRTOS — the same 5 DIDs / 2 DTCs / 3 routines
 *          as basic_ecu, served over DoIP (ISO 13400-2) on FreeRTOS + LwIP.
 *
 *          This is the FreeRTOS counterpart to basic_ecu_doip (Zephyr).
 *          The UDS stack, DoIP server logic, and platform abstraction layer
 *          are identical. Only the OS primitives and IP stack differ.
 *
 *          INTEGRATION SEQUENCE (5 steps):
 *
 *            1. eds_platform_init()                     — CAN transport + NVM
 *            2. uds_generated_init(NULL, 0, 0)          — UDS stack (DoIP-only,
 *                                                         no CAN transport)
 *            2.5. freertos_exec_start(...)              — [EDS#197] the diagnostic
 *                                                         executor task, the one
 *                                                         owner of the UDS server:
 *                                                         1 ms ticks [EDS#191] (S3,
 *                                                         lockout) and DoIP's lock
 *                                                         callbacks (it lends the
 *                                                         server until EDS#197 J6)
 *            3. eds_doip_platform_start_freertos(...)   — DoIP server task
 *            4. vTaskStartScheduler()
 *
 *          Note: can = NULL in step 2 because transport: doip is set in
 *          diagnostics_config.yaml. The EDS_DOIP_ONLY_BUILD guard in
 *          generated/uds_init.c skips isotp_init() when can is NULL.
 *
 *          For production: replace lwip_netif_add() / dhcp stubs below with
 *          your MCU's Ethernet driver initialisation. The DoIP server task
 *          will start listening on port 13400 once the network is up.
 *
 * TARGET : Any FreeRTOS + LwIP Ethernet-capable MCU (STM32H7, i.MX RT, etc.)
 *          CI target: QEMU ARM Cortex-M4 (compile-only; no LwIP in CI)
 *
 * SAFETY  : ASIL-B candidate.
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"

/* EDS stack */
#include "platform_api.h"
#include "uds_init.h"
#include "uds_server.h"
#include "uds_periodic.h"
#include "uds_session.h"
#include "generated_config.h"
#include "uds_security_algo.h"
#include "freertos_exec.h"    /* [EDS#197] the diagnostic executor */

/* DoIP platform */
#include "platform_doip.h"
#include "doip_server.h"

/* Board support (QEMU mps2-an386) */
#include "board_uart.h"
#if defined(EDS_LWIP_REAL) && (EDS_LWIP_REAL == 1)
#include "eds_net.h"
#endif

#include <stdint.h>
#include <stdbool.h>

/* =============================================================================
 * DoIP ECU configuration
 * ============================================================================= */

#define DOIP_ECU_LOGICAL_ADDR   (0xE400U)  /**< Standard xaloqi-tester ECU addr */
#define DOIP_TASK_STACK_BYTES   (4096U)
#define DOIP_TASK_PRIORITY      (6U)

/* [EDS#197] The one producer: DoIP. No periodic TX (decision E: 0x2A over
 * DoIP answers NRC 0x22 until EDS#546). */
static const uds_exec_producer_cfg_t s_producers[] = {
    { .src = (uint8_t)EDS_DOIP_UDS_SRC, .periodic_tx = false },
};

static void s_on_session_change(uds_session_type_t old_sess,
                                uds_session_type_t new_sess)
{
    (void)old_sess;
    if (new_sess == UDS_SESSION_DEFAULT) {
        (void)uds_periodic_cancel_all();
    }
}

/* =============================================================================
 * main() — four-step integration
 * ============================================================================= */

int main(void)
{
    uds_status_t      status;
    uds_server_ctx_t *srv = NULL;

    /*
     * Step 0a: Board bring-up. UART first, so every later step has somewhere
     * to report to. These lines are the CI leg's boot evidence — see
     * compat-tests COVERAGE.md.
     */
    board_uart_init();
    board_uart_puts("\r\n[eds] basic_ecu_doip_freertos boot\r\n");
    board_uart_puts("[eds] board=qemu mps2-an386 cortex-m4f\r\n");

    /*
     * Step 0: Security algorithm placeholder.
     * Inject TRNG callback and OEM keys before production.
     */
    (void)uds_security_algo_set_rng_cb(NULL);

    /*
     * Step 1: Platform init.
     * DoIP-only build — pass NULL for CAN transport. No CAN driver needed.
     * The EDS platform layer is still initialised for NVM and session state.
     */
    status = eds_platform_init(&(eds_platform_cfg_t){
        .can_send            = NULL,   /* no CAN in DoIP-only build */
        .nvm                 = { NULL, NULL, NULL },
        .uds_task_stack_size = 0U,     /* no UDS CAN poll task */
        .uds_task_priority   = 0U,
    });
    if (status != UDS_STATUS_OK) {
        board_uart_puts("[eds] FATAL: eds_platform_init failed\r\n");
        for (;;) { }
    }
    board_uart_puts("[eds] platform init ok\r\n");

    /*
     * Step 2: UDS stack init.
     * NULL CAN transport — EDS_DOIP_ONLY_BUILD guard in uds_init.c
     * skips isotp_init(). UDS server, session, security, DIDs, DTCs
     * and routines are all initialised normally.
     */
    status = uds_generated_init(NULL, 0U, 0U);
    if (status != UDS_STATUS_OK) {
        board_uart_puts("[eds] FATAL: uds_generated_init failed\r\n");
        for (;;) { }
    }

    srv = uds_generated_get_server();
    if (srv == NULL) {
        board_uart_puts("[eds] FATAL: uds_generated_get_server returned NULL\r\n");
        for (;;) { }
    }
    board_uart_puts("[eds] uds stack init ok\r\n");

    (void)uds_periodic_init();
    (void)uds_session_register_change_cb(srv->cfg.session_ctx,
                                          s_on_session_change);

    /*
     * Step 2.5 [EDS#197]: start the diagnostic executor (a static task)
     * before the DoIP server task, so S3/lockout timing is live from the
     * moment a connection can arrive. It replaces the tick task and the
     * EDS#353 lock pair: until DoIP posts its requests to it, DoIP's lock
     * callbacks borrow the server from it (freertos_exec_lend()).
     */
    {
        const freertos_exec_cfg_t ec = {
            .srv            = srv,
            .producers      = s_producers,
            .producer_count = (uint8_t)(sizeof(s_producers) / sizeof(s_producers[0])),
            .reset          = NULL,   /* the DoIP server performs 0x11 (J6 moves it) */
            .after_pass     = NULL,   /* no watchdog on this board */
            .lend_src       = (uint8_t)EDS_DOIP_UDS_SRC,
        };

        if (freertos_exec_start(&ec) != UDS_STATUS_OK) {
            board_uart_puts("[eds] FATAL: freertos_exec_start failed\r\n");
            for (;;) { }
        }
    }
    eds_doip_set_lock_callbacks(freertos_exec_lend, freertos_exec_lend_end);
    /* [EDS#392] Vehicle discovery, on by default (EDS_DOIP_DISCOVERY),
     * configured from ecu.doip.discovery (generated, VIN via the 0xF190
     * safety wrapper). */
    eds_doip_set_discovery(uds_generated_doip_discovery());

    /*
     * Step 2.75: Bring up the IP stack.
     *
     * Only present in the real-lwIP build (-DLWIP_DIR=...). The default
     * build compiles against boards/qemu_cortex_m4/lwip_stub, whose socket
     * calls all return -1 by design, and has no netif to bring up.
     *
     * eds_net_start() only *creates* the bring-up task; lwIP's own
     * tcpip_init() waits on a semaphore for its thread to come up, which
     * would deadlock if it ran here, before vTaskStartScheduler(). So the
     * real work happens in that task once the scheduler is running. The
     * DoIP server task's existing 500 ms delay covers the ordering.
     */
#if defined(EDS_LWIP_REAL) && (EDS_LWIP_REAL == 1)
    eds_net_start();
#endif

    /*
     * Step 3: Start DoIP server task.
     * The task is created here but blocks in vTaskDelay(500ms) before
     * calling lwip_listen — giving LwIP time to come up after the
     * scheduler starts.
     */
    status = eds_doip_platform_start_freertos(
        DOIP_ECU_LOGICAL_ADDR,
        DOIP_PORT,
        srv,
        DOIP_TASK_STACK_BYTES,
        DOIP_TASK_PRIORITY
    );
    if (status != UDS_STATUS_OK) {
        board_uart_puts("[eds] FATAL: doip platform start failed\r\n");
        for (;;) { }
    }
    board_uart_puts("[eds] doip task created, starting scheduler\r\n");

    /*
     * Step 4: Hand control to the FreeRTOS scheduler.
     * The DoIP server task, UDS session timer task (if any), and any
     * application tasks all run from here.
     */
#if defined(DIAG_DOIP_SELFTEST_SESSION_END)
    /* [EDS#197] Test only: tests/freertos_doip_selftest. */
    {
        extern void doip_selftest_start(void);
        doip_selftest_start();
    }
#endif

    vTaskStartScheduler();

    /* Should never reach here */
    for (;;) {}
    return 0;
}
