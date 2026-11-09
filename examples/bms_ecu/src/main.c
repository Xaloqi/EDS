// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/bms_ecu/src/main.c
 *
 * PURPOSE: Battery Management System (BMS) ECU — application entry point.
 *
 * TARGET:  Generic automotive passenger-car BMS ECU.
 *          Tested on native_sim and nucleo_h743zi (STM32H743ZI Cortex-M7).
 *          Portable to any Zephyr-supported board with CAN peripheral.
 *
 * ECU ROLE:
 *   This example models the main controller ECU of a passenger-car BMS for
 *   a 96s Li-Ion pack (8 groups of 12 cells). The BMS is responsible for:
 *     - Cell voltage monitoring (4 group voltage DIDs, per-group aggregation)
 *     - Pack voltage and current measurement
 *     - Temperature monitoring (NTC sensors: cell stack, coolant, PCB)
 *     - State-of-Charge (SoC) and State-of-Health (SoH) estimation
 *     - Contactor control (main +/-, pre-charge) and weld detection
 *     - Passive and active cell balancing
 *     - Isolation monitoring (IMD interface)
 *     - Fault flag bitmask (quick triage DID) and DTC logging
 *
 * DIAGNOSTICS OVERVIEW:
 *   24 DIDs generated from examples/bms_ecu/diagnostics_config.yaml.
 *   All DID handlers are ASIL-B wrapped (5-step validation chain).
 *   10 DTCs covering all safety-relevant BMS faults.
 *   5 RoutineControl procedures: SelfTest, ForcePassiveBalance,
 *     ContactorFunctionalTest, ResetSoCToFullCharge, ClearFaultHistory.
 *   Diagnostic test suite: 24 per-DID test files + firmware integration tests.
 *   Total generated test artifacts: 31 files in generated/tests/.
 *
 * DID HANDLER PATTERN:
 *   The live DIDs are "source: app" in diagnostics_config.yaml: codegen emits
 *   no handler body and src/bms_sim.c defines did_read_<name>() /
 *   did_write_<name>() (declared in generated/did_handlers.h). The registration
 *   table and the 5-step safety wrapper call them, so session, security and
 *   length are checked before they run. The identity DIDs (0xF1xx) keep their
 *   generated default values. bms_sim.c returns fixed boot values; replace
 *   them with real sensor reads, CAN signal decoding or NVM accesses.
 *
 * THREAD MODEL:
 *   main()       — platform init → UDS init → launch diag_task → exit
 *   diag_task    — 1 ms poll loop: CAN RX → ISO-TP → UDS dispatch → CAN TX
 *
 *   The DID handlers run on diag_task, so bms_sim.c needs no lock. If you add
 *   threads that update the pack state (cell monitor IC, current sensor, NTC
 *   ADC, IMD), guard it.
 *
 * STACK BUDGET:
 *   CONFIG_DIAG_TASK_STACK_SIZE = 6144 bytes.
 *   BMS has 24 DIDs + 5 routines; increased vs. basic_ecu (4096) to
 *   accommodate the larger handler dispatch table and SoC estimation callbacks.
 *
 * BUILDING (simulation, no hardware needed):
 *   west build -b native_sim examples/bms_ecu \
 *     -- -DDIAG_SKIP_CODEGEN=ON \
 *     -DDTC_OVERLAY_FILE=examples/bms_ecu/boards/native_sim/native_sim.overlay
 *   ./build/zephyr/zephyr.exe
 *
 * SAFETY   : ASIL-B candidate (ASIL-D target for production BMS).
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

/* --------------------------------------------------------------------------
 * Generated headers (from diagnostics_config.yaml via codegen.py)
 * -------------------------------------------------------------------------- */
#include "uds_init.h"
#include "generated_config.h"
#include "did_handlers.h"
#include "bms_sim.h"

/* --------------------------------------------------------------------------
 * Zephyr headers
 * -------------------------------------------------------------------------- */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bms_ecu, LOG_LEVEL_INF);

/* =============================================================================
 * Configuration
 * ============================================================================= */

#define DIAG_CAN_DEV         DEVICE_DT_GET(DT_ALIAS(can0))
#define DIAG_RX_CAN_ID       ((uint32_t)GEN_CAN_RX_ID)   /* 0x7E0, physical */
#define DIAG_TX_CAN_ID       ((uint32_t)GEN_CAN_TX_ID)   /* 0x7E8: physical    */

#ifndef CONFIG_DIAG_TASK_STACK_SIZE
#define CONFIG_DIAG_TASK_STACK_SIZE   (6144U)
#endif

#ifndef CONFIG_DIAG_TASK_PRIORITY
#define CONFIG_DIAG_TASK_PRIORITY     (5)
#endif

/* Log a warning once every N overrun events, not every tick. */
#define DIAG_OVERRUN_LOG_THRESHOLD    (3U)

/* =============================================================================
 * Diagnostics Infrastructure
 *
 * [NEW-M2 FIX] This section replaces the stale diagnostics wiring that used
 * non-existent API functions from an earlier prototype version:
 *
 *   OLD (broken):                        NEW (current API):
 *   ─────────────────────────────────    ──────────────────────────────────────
 *   zephyr_port_init(can_dev)            zephyr_port_init(&port_cfg, &can)
 *   can_transport_get_instance()         can comes from zephyr_port_init output
 *   zephyr_can_init(t, dev, rx, tx)      replaced by zephyr_port_init
 *   isotp_init(&ch, transport)           handled internally by uds_generated_init
 *   uds_generated_init(&s_isotp_ch)      uds_generated_init(can, rx_id, tx_id)
 *   uds_server_poll()                    can_transport_receive + isotp_process_rx_frame
 *                                        + uds_server_process_request
 *
 * The pattern mirrors examples/basic_ecu/src/main.c exactly.
 * All BMS application state and DID/routine handlers above are unchanged.
 * ============================================================================= */

/* ── Static allocations ────────────────────────────────────────────────────── */

K_THREAD_STACK_DEFINE(s_diag_stack, CONFIG_DIAG_TASK_STACK_SIZE);
static struct k_thread s_diag_thread;

/* UDS message buffers — file scope to avoid stack exhaustion in thread. */
static uds_msg_buf_t s_req_buf;
static uds_msg_buf_t s_resp_buf;

/* Platform objects. */
static diag_mutex_t s_session_lock;  /**< Protects uds_session_ctx_t access.  */
static diag_mutex_t s_security_lock; /**< Protects uds_security_ctx_t access. */
static diag_timer_t s_tick_timer;    /**< 1 ms periodic tick.                 */
static diag_wdt_t   s_wdt;          /**< Hardware watchdog.                  */

/* ── Zephyr TRNG callback for UDS SecurityAccess [P1-SEC] ─────────────────── */
/*
 * Guard the entropy driver header and device handle on CONFIG_ENTROPY_GENERATOR.
 * When CONFIG_ENTROPY_GENERATOR=n (native_sim CI), the LFSR fallback inside
 * uds_security_algo.c is used automatically.
 */
#if defined(CONFIG_ENTROPY_GENERATOR)
#include <zephyr/drivers/entropy.h>
static const struct device *s_entropy_dev = NULL;

static uds_status_t diag_trng_cb(uint8_t *buf, uint8_t len)
{
    if ((s_entropy_dev == NULL) || !device_is_ready(s_entropy_dev)) {
        return UDS_STATUS_ERR_PLATFORM;
    }
    int rc = entropy_get_entropy(s_entropy_dev, buf, (size_t)len);
    return (rc == 0) ? UDS_STATUS_OK : UDS_STATUS_ERR_PLATFORM;
}
#endif /* CONFIG_ENTROPY_GENERATOR */

/**
 * @brief Initialise TRNG (if available) and register callback.
 *
 * Must be called before uds_generated_init() so that the RNG callback is
 * in place before the first SID 0x27 seed request.
 */
static int diag_security_algo_init(void)
{
#if defined(CONFIG_ENTROPY_GENERATOR)
    s_entropy_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
    if (!device_is_ready(s_entropy_dev)) {
        LOG_WRN("[BMS][P1-SEC] TRNG device not ready — using LFSR fallback (dev only)");
        s_entropy_dev = NULL;
    } else {
        uds_security_algo_set_rng_cb(diag_trng_cb);
        LOG_INF("[BMS][P1-SEC] TRNG registered: %s", s_entropy_dev->name);
    }
#else
    LOG_WRN("[BMS][P1-SEC] No zephyr_entropy DTS node — LFSR seed fallback active. "
            "Add RNG DTS node and CONFIG_ENTROPY_GENERATOR=y for production.");
#endif /* CONFIG_ENTROPY_GENERATOR */

    /*
     * OEM KEY INJECTION PLACEHOLDER — replace before production.
     * See the Security Integration Guide (Professional tier — xaloqi.com)
     * for the full key provisioning workflow.
     */
    LOG_WRN("[BMS][P1-SEC] Using compile-time placeholder AES keys. "
            "Inject OTP keys before production.");
    return 0;
}

/* ── ISO-TP RX completion callback ────────────────────────────────────────── */
/*
 * Called from diag_task_entry() when isotp_process_rx_frame() has
 * reassembled a complete UDS PDU into a contiguous buffer.
 *
 * Acquires both session and security locks, dispatches to the UDS server,
 * and transmits the response via ISO-TP.
 *
 * TIMING: Must complete within GEN_P2_SERVER_MAX_MS (25 ms).
 */
static void on_isotp_rx_complete(
    const uint8_t *data,
    uint32_t       length,
    void          *arg)
{
    uds_server_ctx_t *srv    = (uds_server_ctx_t *)arg;
    isotp_ctx_t      *tp     = uds_generated_get_isotp();
    uds_status_t      status;
    uint16_t          i;

    if ((data == NULL) || (srv == NULL) || (tp == NULL)) {
        LOG_ERR("[BMS] on_isotp_rx_complete: NULL argument — PDU dropped.");
        return;
    }

    if ((length == (uint32_t)0U) || (length > (uint32_t)UDS_MAX_PAYLOAD_LEN)) {
        LOG_WRN("[BMS] on_isotp_rx_complete: invalid length %u — PDU dropped.",
                (unsigned)length);
        return;
    }

    for (i = (uint16_t)0U; i < (uint16_t)length; i++) {
        s_req_buf.data[i] = data[i];
    }
    s_req_buf.length  = (uint16_t)length;
    s_resp_buf.length = (uint16_t)0U;

    LOG_DBG("[BMS] UDS RX: SID=0x%02X len=%u",
            (unsigned)data[0], (unsigned)length);

    (void)diag_mutex_lock(&s_session_lock);
    (void)diag_mutex_lock(&s_security_lock);
    /* [EDS-toolchain#165] A functional request (ISO 15765-4) must not draw
     * the NRCs ISO 14229-1 Table 4 says to suppress. */
    uds_addressing_t addressing = UDS_ADDRESSING_PHYSICAL;
    (void)isotp_get_rx_addressing(tp, &addressing);
    status = uds_server_process_request_ex(srv, &s_req_buf, &s_resp_buf, addressing);
    (void)diag_mutex_unlock(&s_security_lock);
    (void)diag_mutex_unlock(&s_session_lock);

    if ((status != UDS_STATUS_OK) && (s_resp_buf.length == (uint16_t)0U)) {
        LOG_ERR("[BMS] UDS server error 0x%02X — no response generated.",
                (unsigned)status);
        return;
    }

    if (s_resp_buf.length > (uint16_t)0U) {
        LOG_DBG("[BMS] UDS TX: RSID=0x%02X len=%u",
                (unsigned)s_resp_buf.data[0], (unsigned)s_resp_buf.length);
        status = isotp_transmit(tp, s_resp_buf.data, s_resp_buf.length);
        if (status != UDS_STATUS_OK) {
            LOG_ERR("[BMS] ISO-TP TX error 0x%02X", (unsigned)status);
        }
    }

    /* [P2-0x11-01] Deferred reset: response is on the wire, now reset. */
    if (srv->pending_reset_type != (uint8_t)0U) {
        uint8_t reset_type = srv->pending_reset_type;
        srv->pending_reset_type = (uint8_t)0U;
        (void)zephyr_port_nvm_flush();
        k_msleep(50);  /* Allow ISO-TP TX to reach the wire before reset (As timer). */
        (void)zephyr_port_ecu_reset(reset_type);
        /* Does not return. */
    }
}

/* ── 1 ms tick callback ────────────────────────────────────────────────────── */

typedef struct tick_ctx {
    uds_server_ctx_t *srv;
    isotp_ctx_t      *tp;
} tick_ctx_t;

static void on_tick(void *arg)
{
    tick_ctx_t *ctx = (tick_ctx_t *)arg;

    if ((ctx == NULL) || (ctx->tp == NULL) || (ctx->srv == NULL)) {
        return;
    }

    (void)isotp_tick_1ms(ctx->tp);

    (void)diag_mutex_lock(&s_session_lock);
    (void)diag_mutex_lock(&s_security_lock);
    (void)uds_server_tick_1ms(ctx->srv);
    (void)uds_periodic_tick_1ms();
    (void)diag_mutex_unlock(&s_security_lock);
    (void)diag_mutex_unlock(&s_session_lock);
}

/* =============================================================================
 * Diagnostics Task
 *
 * Runs as a dedicated Zephyr thread at CONFIG_DIAG_TASK_PRIORITY.
 * Loops at ~1 ms intervals driven by the k_timer-backed diag_timer_t.
 *
 * Each iteration:
 *   1. Wait for 1 ms tick (blocks on semaphore from timer ISR).
 *   2. Poll CAN RX queue — forward frames to ISO-TP reassembler.
 *   3. Drive ISO-TP + UDS timers via on_tick() callback.
 *   4. Feed hardware watchdog.
 *   5. Check for overruns and log if threshold exceeded.
 *
 * Thread args (p1, p2, p3) match the k_thread_create() call in main():
 *   p1 — uds_server_ctx_t *srv
 *   p2 — can_transport_t  *can
 *   p3 — isotp_ctx_t      *tp
 * ============================================================================= */
static void diag_task_entry(void *p1, void *p2, void *p3)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)p1;
    can_transport_t  *can = (can_transport_t  *)p2;
    isotp_ctx_t      *tp  = (isotp_ctx_t      *)p3;

    uds_status_t    status;
    uds_can_frame_t rx_frame;
    bool            frame_ready;
    uint32_t        overrun_count;
    uint32_t        prev_overrun = 0U;

    tick_ctx_t tick_ctx = { .srv = srv, .tp = tp };

    LOG_INF("[BMS] Diagnostics task started (stack %u bytes, priority %d).",
            (unsigned)CONFIG_DIAG_TASK_STACK_SIZE,
            (int)CONFIG_DIAG_TASK_PRIORITY);

    /* Start 1 ms periodic timer — callback is on_tick, arg is tick_ctx. */
    if (diag_timer_start(&s_tick_timer) != UDS_STATUS_OK) {
        LOG_ERR("[BMS] Failed to start 1 ms timer — aborting diag task.");
        return;
    }

    while (true) {

        /* ── [1] Wait for 1 ms tick ──────────────────────────────────────── */
        status = diag_timer_wait_tick(&s_tick_timer, 2U);
        if (status == UDS_STATUS_ERR_TIMEOUT) {
            LOG_WRN("[BMS] 1 ms tick timeout — continuing.");
        }

        /* ── [2] CAN RX poll ─────────────────────────────────────────────── */
        /* [EDS#558] Drain every queued frame, at most the queue depth. The Flow
         * Control BlockSize keeps a burst within the RX queue only if each
         * iteration empties it; one frame per tick loses CFs of long requests. */
        frame_ready = true;
        for (uint32_t rx_n = 0U;
             frame_ready && (rx_n < (uint32_t)EDS_CAN_RX_QUEUE_DEPTH); rx_n++) {
            frame_ready = false;
            status = can_transport_receive(can, &rx_frame, &frame_ready);

            if (status != UDS_STATUS_OK) {
                if (status == UDS_STATUS_ERR_CAN_BUS_OFF) {
                    LOG_ERR("[BMS] CAN bus-off detected.");
                    /*
                     * TODO [APPLICATION]: Implement bus-off recovery.
                     * For now log and continue — WDT will reset if bus stays off.
                     */
                } else {
                    LOG_DBG("[BMS] can_transport_receive error: 0x%02X",
                            (unsigned)status);
                }
            } else if (frame_ready) {
                status = isotp_process_rx_frame(
                    tp,
                    &rx_frame,
                    on_isotp_rx_complete,
                    (void *)srv
                );
                if ((status != UDS_STATUS_OK) &&
                    (status != (uds_status_t)UDS_STATUS_ERR_TP_FRAME_INVALID)) {
                    LOG_WRN("[BMS] ISO-TP RX error: 0x%02X", (unsigned)status);
                }
            } else {
                /* No frame this iteration — idle. */
            }
        }

        /* ── [3] 1 ms tick processing (ISO-TP + UDS + periodic timers) ─── */
        on_tick(&tick_ctx);

        /* ── [3a] Drain periodic push frames due this tick ───────────────── */
        {
            static uds_msg_buf_t s_periodic_frame;
            while (uds_periodic_pop_due(&s_periodic_frame) == UDS_STATUS_OK) {
                /* [EDS#546] A periodic message is a UUDT frame on its own CAN ID
                 * (ISO 14229-3 §8.5): it bypasses the ISO-TP channel, so it can
                 * go out while a multi-frame response is being sent.
                 * [EDS#194] Re-queue if the CAN driver refuses it instead of
                 * silently dropping this period; retry on the next 1ms tick and
                 * stop draining this tick. */
                if (isotp_transmit_uudt(tp, s_periodic_frame.data,
                                   (uint32_t)s_periodic_frame.length) != UDS_STATUS_OK) {
                    (void)uds_periodic_requeue_last();
                    break;
                }
            }
        }

        /* ── [4] Watchdog feed ───────────────────────────────────────────── */
        (void)diag_wdt_feed(&s_wdt);

        /* ── [5] Overrun detection ───────────────────────────────────────── */
        (void)diag_timer_pending_ticks(&s_tick_timer, &overrun_count);
        if (overrun_count > prev_overrun) {
            uint32_t new_overruns = overrun_count - prev_overrun;
            if (new_overruns >= DIAG_OVERRUN_LOG_THRESHOLD) {
                LOG_WRN("[BMS] Poll loop overrun: %u ticks missed.",
                        (unsigned)new_overruns);
            }
            prev_overrun = overrun_count;
        }

    } /* while (true) */
}

static void s_on_session_change(uds_session_type_t old_sess,
                                uds_session_type_t new_sess)
{
    (void)old_sess;
    if (new_sess == UDS_SESSION_DEFAULT) {
        (void)uds_periodic_cancel_all();
    }
}

/* =============================================================================
 * main() — Platform + stack initialisation, then start diagnostics thread.
 *
 * main() exits after starting the diagnostics thread. The Zephyr scheduler
 * takes over and the diagnostics thread runs indefinitely.
 *
 * [NEW-M2 FIX] Replaced stale 4-step manual CAN/ISO-TP wiring with the
 * current API:
 *   1. zephyr_port_init(&port_cfg, &can)   — CAN platform + filter install
 *   2. uds_generated_init(can, rx, tx)     — full stack init (includes ISO-TP)
 *   3. uds_generated_get_server() / get_isotp() — retrieve context pointers
 *   4. Pass srv/can/tp to diagnostics thread as p1/p2/p3
 * ============================================================================= */
int main(void)
{
    uds_status_t      st;
    can_transport_t  *can  = NULL;
    uds_server_ctx_t *srv  = NULL;
    isotp_ctx_t      *tp   = NULL;
    zephyr_port_cfg_t port_cfg;

    LOG_INF("=== BMS ECU — Xaloqi EDS v1.0.0 ===");
    LOG_INF("    24 DIDs | 10 DTCs | 5 Routines | ASIL-B");

    /* --- 1. Initialise BMS application state ----------------------------- */
    bms_sim_init();
    LOG_INF("[BMS] Application state initialised.");

    /* --- 2. Platform objects init --------------------------------------- */
    if (diag_mutex_init(&s_session_lock) != UDS_STATUS_OK) {
        LOG_ERR("[BMS] Session mutex init failed.");
        return -EIO;
    }
    if (diag_mutex_init(&s_security_lock) != UDS_STATUS_OK) {
        LOG_ERR("[BMS] Security mutex init failed.");
        return -EIO;
    }

    st = diag_wdt_init(&s_wdt);
    if (st != UDS_STATUS_OK) {
        /* Non-fatal — continue without WDT for CI/simulation. */
        LOG_WRN("[BMS] WDT init returned 0x%02X (non-fatal).", (unsigned)st);
    }

    /*
     * 1 ms timer: register on_tick callback now; start is deferred to
     * the diagnostics thread after full stack init completes.
     */
    st = diag_timer_init(&s_tick_timer, on_tick, NULL);
    if (st != UDS_STATUS_OK) {
        LOG_ERR("[BMS] Timer init failed: 0x%02X", (unsigned)st);
        return -EIO;
    }

    /* --- 3. Platform + CAN transport ------------------------------------ */
    const struct device *can_dev = DIAG_CAN_DEV;
    if (!device_is_ready(can_dev)) {
        LOG_ERR("[BMS] CAN device not ready — diagnostics disabled.");
        return -EIO;
    }

    (void)memset(&port_cfg, 0, sizeof(port_cfg));
    port_cfg.can_dev        = can_dev;
    port_cfg.physical_rx_id = DIAG_RX_CAN_ID;  /* physical request ID; the
                                     * 0x7DF functional filter is always installed */

    st = zephyr_port_init(&port_cfg, &can);
    if (st != UDS_STATUS_OK) {
        LOG_ERR("[BMS] zephyr_port_init failed: 0x%02X", (unsigned)st);
        return -EIO;
    }
    LOG_INF("[BMS] CAN transport ready (device: %s).", can_dev->name);

    /* --- 3.5. NVM store initialization ------------------------------------ */
    /*
     * [EDS#200] Must run before uds_generated_init() (dtc_mirror_init() at
     * Step 3.5 sits on top of nvm_store and silently no-ops until this
     * succeeds). This example only targets native_sim, where
     * platform/zephyr/nvm_store_mock.c (RAM-backed) is linked instead of
     * the real NVS backend — its nvm_store_init() ignores cfg entirely, so
     * NULL is correct here (see nvm_store.h: "may be NULL on host").
     */
    st = nvm_store_init(NULL);
    if (st != UDS_STATUS_OK) {
        LOG_ERR("[BMS] NVM store init failed: 0x%02X — DTC mirror will not "
                "survive reset this run.", (unsigned)st);
    }

    /* --- 4. Security algorithm init ------------------------------------- */
    /*
     * Must run before uds_generated_init() so that the TRNG callback and
     * any injected OEM keys are in place before the first seed request.
     */
    (void)diag_security_algo_init();

    /* --- 5. UDS stack initialisation (generated from YAML) -------------- */
    st = uds_generated_init(can, DIAG_RX_CAN_ID, DIAG_TX_CAN_ID);
    if (st != UDS_STATUS_OK) {
        LOG_ERR("[BMS] uds_generated_init failed: 0x%02X", (unsigned)st);
        return -EIO;
    }

    srv = uds_generated_get_server();
    tp  = uds_generated_get_isotp();

    if ((srv == NULL) || (tp == NULL)) {
        LOG_ERR("[BMS] Context accessors returned NULL after init.");
        return -EIO;
    }

    (void)uds_periodic_init();
    (void)uds_session_register_change_cb(srv->cfg.session_ctx,
                                          s_on_session_change);

    LOG_INF("[BMS] UDS stack initialised. 24 DIDs / 10 DTCs registered.");
    LOG_INF("[BMS]   RX CAN: 0x%03X  TX CAN: 0x%03X",
            (unsigned)DIAG_RX_CAN_ID, (unsigned)DIAG_TX_CAN_ID);

    /* --- 6. Launch diagnostics polling task ----------------------------- */
    /*
     * Pass srv (p1), can (p2), tp (p3) as thread arguments.
     * diag_task_entry() uses these directly — no global state needed.
     */
    (void)k_thread_create(
        &s_diag_thread,
        s_diag_stack,
        K_THREAD_STACK_SIZEOF(s_diag_stack),
        diag_task_entry,
        (void *)srv,
        (void *)can,
        (void *)tp,
        CONFIG_DIAG_TASK_PRIORITY,
        (uint32_t)0U,
        K_NO_WAIT
    );
    k_thread_name_set(&s_diag_thread, "bms_diag");

    LOG_INF("[BMS] Diagnostics task launched. ECU ready for scan-tool.");

    /* main() returns — Zephyr scheduler takes over. */
    return 0;
}
