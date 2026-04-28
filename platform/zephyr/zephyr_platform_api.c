// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/zephyr_platform_api.c
 *
 * PURPOSE: Implements platform/platform_api.h using Zephyr RTOS APIs.
 *
 *          This file provides the eds_platform_* functions for Zephyr builds.
 *          The eds_platform_init() function is a no-op on Zephyr — DTS and
 *          Kconfig wire CAN and NVM. The reset and NVM flush implementations
 *          are extracted from the existing zephyr_port.c logic.
 *
 *          RELATIONSHIP TO zephyr_port.c:
 *            zephyr_port.c  — Zephyr-specific startup (CAN init, port init).
 *                             Kept as-is for the Zephyr integration layer.
 *            This file       — eds_platform_* neutral interface implementations.
 *                             Called from platform-neutral code paths.
 *
 * SAFETY  : ASIL-B candidate. Reset and NVM flush are safety-relevant.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "platform_api.h"
#include "uds_types.h"
#include "nvm_store.h"
#include "dtc_mirror.h"
#include "uds_session_stats.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

#include <string.h>

LOG_MODULE_REGISTER(zephyr_platform_api, LOG_LEVEL_INF);

/* --------------------------------------------------------------------------
 * Reset sub-function values (ISO 14229-1 Table 186)
 * -------------------------------------------------------------------------- */
#define ECU_RESET_HARD_RESET   (0x01U)
#define ECU_RESET_KEY_OFF_ON   (0x02U)
#define ECU_RESET_SOFT_RESET   (0x03U)

/* ============================================================================
 * eds_platform_init
 * ========================================================================== */

uds_status_t eds_platform_init(const eds_platform_cfg_t *cfg)
{
    /*
     * Zephyr: no-op. CAN is configured via DTS (DEVICE_DT_GET / can0 alias).
     * NVM is configured via zephyr_port.c / nvm_store_init().
     * The cfg pointer is intentionally unused on this platform.
     */
    (void)cfg;
    return UDS_STATUS_OK;
}

/* ============================================================================
 * eds_platform_ecu_reset
 * ========================================================================== */

uds_status_t eds_platform_ecu_reset(uint8_t reset_type)
{
    /*
     * SAFETY (ISO 14229-1 §11.3.3):
     *   The caller MUST have already:
     *     1. Transmitted the positive response frame via ISO-TP.
     *     2. Waited for N_As TX confirmation.
     *     3. Called eds_platform_nvm_flush().
     *   This function must NOT return on a valid reset type.
     */
    switch (reset_type) {

        case ECU_RESET_HARD_RESET:
            LOG_INF("ECU reset: hard reset (cold).");
            k_msleep(5);   /* Allow LOG flush before reset. */
            sys_reboot(SYS_REBOOT_COLD);
            break;   /* Unreachable — sys_reboot() does not return. */

        case ECU_RESET_KEY_OFF_ON:
            /*
             * Key-off/on: platform-specific power sequencing.
             * Default: cold reboot. OEM integrators replace sys_reboot with
             * their PMIC/power-sequencer command here.
             * TODO [APPLICATION]: Replace with OEM power sequencer.
             */
            LOG_INF("ECU reset: key-off/on (fallback: cold reboot).");
            k_msleep(5);
            sys_reboot(SYS_REBOOT_COLD);
            break;   /* Unreachable */

        case ECU_RESET_SOFT_RESET:
            /*
             * Warm reset — preserves some peripheral state on supported SoCs.
             * Zephyr falls back to cold reset on platforms without warm reset.
             */
            LOG_INF("ECU reset: soft reset (warm).");
            k_msleep(5);
            sys_reboot(SYS_REBOOT_WARM);
            break;   /* Unreachable */

        default:
            /*
             * service_0x11.c validates reset_type before setting
             * pending_reset_type, so this path should never be reached in
             * production. Return an error so the caller can log it.
             */
            LOG_WRN("ECU reset: unsupported type 0x%02X.", (unsigned)reset_type);
            return UDS_STATUS_ERR_PLATFORM;
    }

    /* Unreachable — sys_reboot() does not return. Defensive return. */
    return UDS_STATUS_ERR_PLATFORM;
}

/* ============================================================================
 * eds_platform_nvm_flush
 * ========================================================================== */

uds_status_t eds_platform_nvm_flush(void)
{
    uds_status_t rc;
    uds_status_t final_rc = UDS_STATUS_OK;

    LOG_INF("NVM flush: persisting diagnostic counters before reset.");

    /*
     * [FLUSH-01] DTC status-byte mirror.
     * Serialises all registered DTC status bytes to a single NVS record.
     * Failure: DTC history is lost on reset — logged but non-fatal.
     */
    rc = dtc_mirror_flush_all();
    if (rc != UDS_STATUS_OK) {
        LOG_ERR("NVM flush: DTC mirror write failed (rc=0x%02X)", (unsigned)rc);
        final_rc = rc;
    } else {
        LOG_DBG("NVM flush: DTC mirror OK");
    }

    /*
     * [FLUSH-02] ECU lifecycle counter.
     * Incremented on every ECU reset. Informational only — not safety-relevant.
     */
    if (nvm_store_is_ready()) {
        uint32_t lifecycle_cnt = 0U;
        size_t   read_len      = 0U;

        rc = nvm_store_read(
            (uint16_t)NVM_KEY_LIFECYCLE_CNT,
            &lifecycle_cnt, sizeof(lifecycle_cnt), &read_len);

        if ((rc == UDS_STATUS_OK) || (rc == UDS_STATUS_ERR_DID_NOT_FOUND)) {
            lifecycle_cnt++;
            rc = nvm_store_write(
                (uint16_t)NVM_KEY_LIFECYCLE_CNT,
                &lifecycle_cnt, sizeof(lifecycle_cnt));

            if (rc == UDS_STATUS_OK) {
                LOG_DBG("NVM flush: lifecycle counter = %u",
                        (unsigned)lifecycle_cnt);
            } else {
                LOG_WRN("NVM flush: lifecycle counter write failed (rc=0x%02X)",
                        (unsigned)rc);
            }
        }
    }

    /*
     * [FLUSH-03] Security attempt counter.
     * Written eagerly on each failed attempt (uds_security_send_key →
     * nvm_save_cb). Already current in NVM — no flush needed here.
     */

    /*
     * [FLUSH-04] Session statistics.
     * Dirty-flagged internally — only writes if data changed since last flush.
     */
    rc = uds_session_stats_flush();
    if (rc != UDS_STATUS_OK) {
        LOG_WRN("NVM flush: session stats write failed (rc=0x%02X)", (unsigned)rc);
        /* Non-fatal — informational counters only. */
    } else {
        LOG_DBG("NVM flush: session stats OK");
    }

    return final_rc;
}

/* ============================================================================
 * eds_platform_uptime_ms
 * ========================================================================== */

uint32_t eds_platform_uptime_ms(void)
{
    return (uint32_t)k_uptime_get_32();
}

/* ============================================================================
 * eds_platform_can_input
 * Not used on Zephyr — CAN RX is handled internally by zephyr_can.c via
 * the Zephyr CAN driver callback and k_msgq. This stub satisfies the linker
 * if platform_api.h is included in a Zephyr translation unit.
 * ========================================================================== */

void eds_platform_can_input(const eds_can_frame_t *frame)
{
    /*
     * Zephyr: no-op. Zephyr CAN driver calls rx_callback() → k_msgq_put()
     * directly. This function exists only to satisfy the platform_api.h
     * contract for FreeRTOS builds. It is never called on Zephyr.
     */
    (void)frame;
}
