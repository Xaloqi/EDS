// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr_port.c
 *
 * PURPOSE: Zephyr RTOS platform abstraction layer — Phase 6A full implementation.
 *
 * SAFETY  : ASIL-B candidate. ECU reset path is safety-relevant.
 * STANDARD: MISRA C:2012 alignment intended.
 *
 * [NEW-H1 FIX] zephyr_can_platform_init() call updated from 2-argument to
 * 3-argument form, passing cfg->physical_rx_id as the new third argument.
 *
 * Previously this file called:
 *   zephyr_can_platform_init(out_transport, cfg->can_dev);
 *
 * With only 2 arguments, cfg->physical_rx_id was never forwarded to the CAN
 * driver, meaning physical addressing could never be activated regardless of
 * the YAML configuration or the port_cfg value set by the application.
 *
 * The correct 3-argument call is now:
 *   zephyr_can_platform_init(out_transport, cfg->can_dev, cfg->physical_rx_id);
 *
 * This matches the 3-parameter signature in zephyr_can.c / zephyr_can.h and
 * the updated forward declaration in both zephyr_port.h files.
 * =============================================================================
 */

#include "zephyr_port.h"
#include "can_transport.h"
#include "uds_types.h"
#include "nvm_store.h"
#include "dtc_mirror.h"
#include "uds_session_stats.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

/* [FIX] Was LOG_MODULE_DECLARE(basic_ecu) — see platform/zephyr/zephyr_can.c for rationale. */
LOG_MODULE_REGISTER(zephyr_port, LOG_LEVEL_INF);

/* --------------------------------------------------------------------------
 * Reset type sub-function values (ISO 14229-1 Table C.1)
 * -------------------------------------------------------------------------- */
#define ECU_RESET_HARD_RESET       (0x01U)
#define ECU_RESET_KEY_OFF_ON       (0x02U)
#define ECU_RESET_SOFT_RESET       (0x03U)

/* --------------------------------------------------------------------------
 * Public API implementations
 * -------------------------------------------------------------------------- */

uds_status_t zephyr_port_init(
    const zephyr_port_cfg_t *cfg,
    can_transport_t        **out_transport)
{
    uds_status_t status;

    if ((cfg == NULL) || (out_transport == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (cfg->can_dev == NULL) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /*
     * [NEW-H1 FIX] Pass cfg->physical_rx_id as the third argument.
     *
     * Previously called with only 2 arguments (out_transport, cfg->can_dev),
     * which silently discarded the physical_rx_id field and prevented physical
     * addressing from ever being activated.
     *
     * Passing 0U here (when physical_rx_id is not set by the application)
     * keeps the existing behaviour: functional-only mode (0x7DF filter only).
     * Passing a non-zero value (e.g. 0x7E0U) installs a second RX filter for
     * point-to-point ECU addressing as described in the Security Integration
     * Guide (Professional tier — see xaloqi.com).
     */
    status = zephyr_can_platform_init(out_transport,
                                       cfg->can_dev,
                                       cfg->physical_rx_id); /* [P2-3][NEW-H1 FIX] */
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    return UDS_STATUS_OK;
}

uds_timestamp_ms_t zephyr_port_get_time_ms(void)
{
    return (uds_timestamp_ms_t)k_uptime_get_32();
}

void zephyr_port_delay_ms(uint32_t ms)
{
    k_msleep((int32_t)ms);
}

uds_status_t zephyr_port_ecu_reset(uint8_t reset_type)
{
    /*
     * SAFETY: Before resetting, the caller (service 0x11) is responsible for:
     *   1. Transmitting the positive response CAN frame.
     *   2. Waiting for N_As timeout to ensure frame delivery.
     *   3. Calling this function only after those steps complete.
     *
     * This function must NOT return on a valid reset type.
     * Returning UDS_STATUS_ERR_PLATFORM for unsupported types allows
     * the service handler to generate a NRC instead of a silent failure.
     */

    switch (reset_type) {

        case ECU_RESET_HARD_RESET:
            /*
             * Full cold reset — equivalent to power cycling.
             * Clears all volatile state, RAM, and peripheral registers.
             * sys_reboot() does not return.
             */
            LOG_INF("ECU reset: hard reset (cold).");
            k_msleep(5);   /* Allow LOG flush. */
            sys_reboot(SYS_REBOOT_COLD);
            break;   /* Unreachable — silences MISRA-15.5 checker. */

        case ECU_RESET_KEY_OFF_ON:
            /*
             * Key-off/on reset: application-defined power sequencing.
             * On targets without dedicated power control GPIO, fall back
             * to a cold reboot. OEM integration must replace this with
             * platform-specific power sequencing (e.g. PMIC command).
             *
             * TODO [APPLICATION]: Replace sys_reboot with OEM power sequencer.
             */
            LOG_INF("ECU reset: key-off/on (fallback: cold reboot).");
            k_msleep(5);
            sys_reboot(SYS_REBOOT_COLD);
            break;   /* Unreachable */

        case ECU_RESET_SOFT_RESET:
            /*
             * Warm reset — preserves some peripheral state on supported SoCs.
             * sys_reboot(SYS_REBOOT_WARM) is a no-op on platforms that do not
             * support warm reset; Zephyr falls back to cold reset automatically.
             */
            LOG_INF("ECU reset: soft reset (warm).");
            k_msleep(5);
            sys_reboot(SYS_REBOOT_WARM);
            break;   /* Unreachable */

        default:
            /*
             * Unsupported reset type — service 0x11 will have already
             * validated this before calling here. Return an error so the
             * service handler sends a NRC 0x31 (requestOutOfRange).
             */
            LOG_WRN("ECU reset: unsupported type 0x%02X.", (unsigned)reset_type);
            return UDS_STATUS_ERR_PLATFORM;
    }

    /*
     * Unreachable under normal execution (sys_reboot does not return).
     * Return error defensively in case of platform-level failure.
     */
    return UDS_STATUS_ERR_PLATFORM;
}

uint32_t zephyr_port_enter_critical(void)
{
    return (uint32_t)irq_lock();
}

void zephyr_port_exit_critical(uint32_t key)
{
    irq_unlock((unsigned int)key);
}

uds_status_t zephyr_port_nvm_flush(void)
{
    uds_status_t rc;
    uds_status_t final_rc = UDS_STATUS_OK;

    LOG_INF("NVM flush: persisting diagnostic counters before reset.");

    /*
     * [P3-NVM-01] Flush DTC status-byte mirror.
     *
     * Serializes all registered DTC status bytes and writes them as a
     * single atomic NVS record. If this fails, DTC history is lost on
     * reset — logged but non-fatal (reset proceeds regardless).
     */
    rc = dtc_mirror_flush_all();
    if (rc != UDS_STATUS_OK) {
        LOG_ERR("NVM flush: DTC mirror write failed (rc=0x%02X)", (unsigned)rc);
        final_rc = rc;
    } else {
        LOG_DBG("NVM flush: DTC mirror OK");
    }

    /*
     * [P3-NVM-02] Increment and persist the ECU lifecycle counter.
     *
     * The lifecycle counter is incremented on every ECU reset (power-cycle,
     * watchdog, or SID 0x11). Useful for field diagnostics and audit trails.
     *
     * Security note: the counter is informational only. It does not gate
     * any security-critical path and is not safety-relevant.
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
                LOG_DBG("NVM flush: lifecycle counter = %u", (unsigned)lifecycle_cnt);
            } else {
                LOG_WRN("NVM flush: lifecycle counter write failed (rc=0x%02X)", (unsigned)rc);
            }
        }
    }

    /*
     * [P3-NVM-03] Security attempt counter is written eagerly on each failed
     * attempt (in uds_security_send_key → nvm_save_cb). No additional flush
     * needed here — the counter is already current in NVM.
     */

    /*
     * [P3-NVM-04] Flush session statistics (reset count, session entry counts,
     * security event counts). These are dirty-flagged so only write if changed.
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
