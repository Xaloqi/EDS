// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/motor_controller_ecu/src/main.c
 *
 * PURPOSE: Traction Inverter / Motor Controller ECU — application entry point.
 *
 * TARGET:  Generic automotive traction inverter ECU.
 *          Tested on native_sim and nucleo_h743zi (STM32H743ZI Cortex-M7).
 *          Portable to any Zephyr-supported board with CAN peripheral.
 *
 * ECU ROLE:
 *   This example models the main controller ECU of a 3-phase traction inverter
 *   for an IPMSM (interior permanent-magnet synchronous motor). The inverter
 *   is responsible for:
 *     - Field-oriented control (FOC): torque and flux current regulation
 *     - Rotor position / speed sensing via resolver (sin/cos → RDC)
 *     - Phase current measurement (3× inline shunts, ADC @ ≥10 kHz)
 *     - DC-link voltage monitoring and pre-charge supervision
 *     - IGBT / SiC gate-driver health (desaturation detection)
 *     - Junction temperature estimation (T_j = T_case + P_loss × R_th)
 *     - Regenerative braking coordination with BMS over CAN
 *
 *   Together with the bms_ecu example, this forms the complete EV powertrain
 *   diagnostic coverage: BMS (energy storage) + Motor Controller (conversion).
 *
 * DIAGNOSTICS OVERVIEW:
 *   27 DIDs generated from examples/motor_controller_ecu/diagnostics_config.yaml.
 *   10 DTCs covering all safety-relevant inverter protection faults.
 *   6 RoutineControl procedures: SelfTest, PhaseBalanceTest,
 *     GateDriverFunctionalTest, ResolverOffsetCalibration,
 *     ForceMotorInhibit, ClearFaultHistory.
 *   Diagnostic test suite: 27 per-DID + 6 per-routine + 2 service test files.
 *   Total generated test artifacts: 40 files in generated/tests/.
 *
 * DID HANDLERS:
 *   Live in generated/did_handlers.c, registered by did_handlers_register_all()
 *   via uds_generated_init() below. Each returns a deterministic stub value
 *   until replaced with real FOC/sensor/NVM access. [EDS#340]: this file used
 *   to also declare a parallel set of DID read/write and routine handlers
 *   (did_read_F190()/g_mc_state/g_mc_mutex and friends, ~590 lines) that
 *   looked like the real implementation but were never registered with
 *   anything — the did_database_register() calls in generated/uds_init.c
 *   point at the generated snake_case handlers (did_read_mc_rotorspeed_rpm()
 *   etc.), not these. Unreachable dead code with its own incompatible calling
 *   convention (single-argument did_read_XXXX(uint8_t *buf), not the
 *   did_read_cb_fn(buf, buf_len, out_len) the registry actually expects).
 *   Removed rather than left to mislead the next integrator.
 *
 * THREAD MODEL:
 *   main()       — platform init → UDS init → launch diag_task → exit
 *   diag_task    — 1 ms poll loop: CAN RX → ISO-TP → UDS dispatch → CAN TX
 *
 * BUILDING (simulation, no hardware needed):
 *   west build -b native_sim examples/motor_controller_ecu \
 *     -- -DDIAG_SKIP_CODEGEN=ON \
 *     -DDTC_OVERLAY_FILE=examples/motor_controller_ecu/boards/native_sim/native_sim.overlay
 *   ./build/zephyr/zephyr.exe
 *
 * SAFETY   : ASIL-B candidate.
 * STANDARD : MISRA C:2012 alignment intended.
 * LICENSE  : Apache-2.0
 * VERSION  : 1.0.0
 * =============================================================================
 */

/* --------------------------------------------------------------------------
 * Diagnostics stack headers
 * -------------------------------------------------------------------------- */
#include "uds_types.h"
#include "uds_server.h"
#include "uds_security_algo.h"
#include "uds_periodic.h"
#include "uds_session.h"
#include "isotp.h"
#include "can_transport.h"
#include "zephyr_port.h"
#include "zephyr_mutex.h"
#include "zephyr_timer.h"
#include "zephyr_wdt.h"
#include "nvm_store.h"
#include "platform_api.h"   /* eds_platform_uptime_ms() */
#include "eds_elapsed.h"    /* [EDS#426] elapsed-time ticking */

/* --------------------------------------------------------------------------
 * Generated headers (from diagnostics_config.yaml via codegen.py)
 * -------------------------------------------------------------------------- */
#include "uds_init.h"
#include "generated_config.h"
#include "did_handlers.h"

/* --------------------------------------------------------------------------
 * Zephyr headers
 * -------------------------------------------------------------------------- */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>

#include <string.h>

LOG_MODULE_REGISTER(motor_controller_ecu, LOG_LEVEL_INF);

/* =============================================================================
 * Configuration
 * ============================================================================= */

#define DIAG_CAN_DEV         DEVICE_DT_GET(DT_ALIAS(can0))
#define DIAG_RX_CAN_ID       ((uint32_t)GEN_CAN_RX_ID)   /* 0x7DF: functional */
#define DIAG_TX_CAN_ID       ((uint32_t)GEN_CAN_TX_ID)   /* 0x7E8: physical    */

#ifndef CONFIG_DIAG_TASK_STACK_SIZE
#define CONFIG_DIAG_TASK_STACK_SIZE   (6144U)
#endif

#ifndef CONFIG_DIAG_TASK_PRIORITY
#define CONFIG_DIAG_TASK_PRIORITY     (5)
#endif

#define DIAG_OVERRUN_LOG_THRESHOLD    (3U)

/* [EDS#426] Most 1 ms ticks one loop iteration runs. Time beyond it is
 * carried by eds_elapsed_consume() into the next iterations, not lost. */
#define DIAG_TICK_MAX_PER_LOOP_MS     (100U)

/* =============================================================================
 * DTC Registration helpers
 * Called from uds_generated_init() via generated/uds_init.c
 * ============================================================================= */

/* Forward declaration — implemented in generated/uds_init.c */
extern void register_dtcs(void);

/* =============================================================================
 * Diagnostics task (1 ms poll loop)
 * ============================================================================= */

/* [EDS#426] A complete ISO-TP request: dispatch it and send the response.
 * Single diagnostics thread, so no session lock is needed (unlike
 * basic_ecu, where another thread shares uds_server_ctx_t). */
static void on_isotp_rx_complete(const uint8_t *data, uint32_t length, void *arg)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)arg;
    isotp_ctx_t      *tp  = uds_generated_get_isotp();

    /* Static buffers — no stack allocation of uds_msg_buf_t. */
    static uds_msg_buf_t s_req;
    static uds_msg_buf_t s_resp;

    if ((data == NULL) || (srv == NULL) || (tp == NULL)) { return; }
    if ((length == 0U) || (length > (uint32_t)UDS_MAX_PAYLOAD_LEN)) { return; }

    (void)memcpy(s_req.data, data, (size_t)length);
    s_req.length  = (uint16_t)length;
    s_resp.length = 0U;

    (void)uds_server_process_request(srv, &s_req, &s_resp);

    if (s_resp.length > 0U) {
        (void)isotp_transmit(tp, s_resp.data, s_resp.length);
    }
}

static void s_on_session_change(uds_session_type_t old_sess,
                                uds_session_type_t new_sess)
{
    (void)old_sess;
    if (new_sess == UDS_SESSION_DEFAULT) {
        (void)uds_periodic_cancel_all();
    }
}

static void diag_task_fn(void *p1, void *p2, void *p3)
{
    (void)p1; (void)p2; (void)p3;

    const struct device   *can_dev = DIAG_CAN_DEV;
    can_transport_t       *can     = NULL;
    zephyr_port_cfg_t      port_cfg;
    uds_status_t           status;
    uint32_t               overrun_count = 0U;

    /* ── Platform init ────────────────────────────────────────────────────── */
    if (!device_is_ready(can_dev)) {
        LOG_ERR("[MC] CAN device not ready");
        return;
    }

    (void)memset(&port_cfg, 0, sizeof(port_cfg));
    port_cfg.can_dev = can_dev;

    status = zephyr_port_init(&port_cfg, &can);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("[MC] zephyr_port_init failed: 0x%02X", (unsigned)status);
        return;
    }

    /* ── NVM store initialization ────────────────────────────────────────── */
    /*
     * [EDS#200] Must run before uds_generated_init() (dtc_mirror_init() at
     * Step 3.5 sits on top of nvm_store and silently no-ops until this
     * succeeds). This example only targets native_sim, where
     * platform/zephyr/nvm_store_mock.c (RAM-backed) is linked instead of
     * the real NVS backend — its nvm_store_init() ignores cfg entirely, so
     * NULL is correct here (see nvm_store.h: "may be NULL on host").
     */
    status = nvm_store_init(NULL);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("[MC] NVM store init failed: 0x%02X — DTC mirror will not "
                "survive reset this run.", (unsigned)status);
    }

    /* ── UDS stack init ────────────────────────────────────────────────────── */
    status = uds_generated_init(can,
                                 (uint32_t)DIAG_RX_CAN_ID,
                                 (uint32_t)DIAG_TX_CAN_ID);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("[MC] uds_generated_init failed: 0x%02X", (unsigned)status);
        return;
    }

    uds_server_ctx_t *mc_srv = uds_generated_get_server();
    isotp_ctx_t      *mc_tp  = uds_generated_get_isotp();
    if ((mc_srv == NULL) || (mc_tp == NULL)) {
        LOG_ERR("[MC] UDS server or ISO-TP context NULL after init");
        return;
    }
    (void)uds_periodic_init();
    (void)uds_session_register_change_cb(mc_srv->cfg.session_ctx,
                                          s_on_session_change);

    LOG_INF("[MC] Motor controller diagnostics running");
    LOG_INF("[MC] RX=0x%03X TX=0x%03X", DIAG_RX_CAN_ID, DIAG_TX_CAN_ID);

    /* ── 1 ms poll loop ─────────────────────────────────────────────────────
     * [EDS#426] Until this fix the loop never read CAN RX (so no request was
     * ever answered), never ticked ISO-TP, and passed NULL to
     * uds_server_tick_1ms() (so S3 and the security lockout never ran).
     * Ticks follow elapsed time [EDS#411]: one iteration is at least a
     * kernel tick plus the loop's own work, not 1 ms. */
    eds_elapsed_t tick_clock;
    eds_elapsed_init(&tick_clock, eds_platform_uptime_ms());

    while (true) {
        k_msleep(1);

        uds_can_frame_t rx_frame;
        bool            frame_ready = false;
        status = can_transport_receive(can, &rx_frame, &frame_ready);
        if ((status == UDS_STATUS_OK) && frame_ready) {
            (void)isotp_process_rx_frame(mc_tp, &rx_frame,
                                          on_isotp_rx_complete, mc_srv);
        }

        uint32_t elapsed = eds_elapsed_consume(&tick_clock, eds_platform_uptime_ms(),
                                               DIAG_TICK_MAX_PER_LOOP_MS);
        if (elapsed > (1U + DIAG_OVERRUN_LOG_THRESHOLD)) {
            overrun_count++;
            if ((overrun_count % (uint32_t)10U) == (uint32_t)1U) {
                LOG_WRN("[MC] Tick overrun (×%u)", (unsigned)overrun_count);
            }
        }
        for (uint32_t i = 0U; i < elapsed; i++) {
            (void)isotp_tick_1ms(mc_tp);
            (void)uds_server_tick_1ms(mc_srv);
            (void)uds_periodic_tick_1ms();
        }

        {
            static uds_msg_buf_t s_periodic_frame;
            while (uds_periodic_pop_due(&s_periodic_frame) == UDS_STATUS_OK) {
                /* [EDS#194] Re-queue on ISO-TP busy instead of silently dropping
                 * this period -- isotp_transmit() can legitimately return BUSY
                 * mid multi-frame send; retry on the next 1ms tick rather than
                 * losing the period, and stop draining this tick (the same busy
                 * channel would just fail the next pop too). */
                if (isotp_transmit(mc_tp, s_periodic_frame.data,
                                   (uint32_t)s_periodic_frame.length) != UDS_STATUS_OK) {
                    (void)uds_periodic_requeue_last();
                    break;
                }
            }
        }
    }
}

K_THREAD_DEFINE(diag_task, CONFIG_DIAG_TASK_STACK_SIZE,
                diag_task_fn, NULL, NULL, NULL,
                CONFIG_DIAG_TASK_PRIORITY, 0, 0);

/* =============================================================================
 * main()
 * ============================================================================= */

int main(void)
{
    LOG_INF("[MC] Motor Controller ECU — Xaloqi EDS v1.0.0");
    LOG_INF("[MC] 27 DIDs | 10 DTCs | 6 Routines | ASIL-B safety wrappers");

    /* Security algorithm: register TRNG callback before any seed generation.
     * TODO: replace with real TRNG source (e.g. Zephyr entropy driver).
     * The reference implementation uses a software counter as a nonce seed. */
    uds_security_algo_reset();

    LOG_INF("[MC] Application init complete. Diagnostics task starting.");
    /* diag_task starts automatically via K_THREAD_DEFINE */
    return 0;
}
