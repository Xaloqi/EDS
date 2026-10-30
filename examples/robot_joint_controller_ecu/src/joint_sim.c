/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/robot_joint_controller_ecu/src/joint_sim.c
 *
 * PURPOSE: Simulated joint behind DIDs 0xA000-0xA004. See joint_sim.h.
 *
 * MODEL (all integer, no malloc, no recursion, no floating point):
 *   position    triangle wave, +/-90 deg, 20 s per cycle -> 18 deg/s
 *   velocity    +/-3.00 rpm (the slope of the position), sign follows the
 *               direction of travel
 *   torque      0.40 Nm of friction in the direction of travel,
 *               plus a load proportional to the position (0.01 Nm per 5 deg)
 *   temperature 30 degC at boot, +1 degC/s, saturating at 100 degC
 *   status      ENABLED | HOME_COMPLETE; FAULT_ACTIVE | OVERTEMP once the
 *               temperature exceeds 85 degC
 *   DTC         0xA00101 is set while the temperature exceeds 85 degC
 *
 * ENCODINGS (diagnostics_config.yaml):
 *   0xA000 int32 BE, 0.001 deg      0xA001 int16 BE, 0.01 rpm
 *   0xA002 int16 BE, 0.01 Nm        0xA003 uint8, raw = degC + 40
 *   0xA004 uint8 bitmask
 *
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

#include "joint_sim.h"

#include "did_handlers.h"
#include "dtc_database.h"
#include "uds_types.h"

#include <stdbool.h>
#include <stddef.h>

/* 0xA004 bits (diagnostics_config.yaml). */
#define STATUS_ENABLED          (0x01U)
#define STATUS_FAULT_ACTIVE     (0x04U)
#define STATUS_HOME_COMPLETE    (0x08U)
#define STATUS_OVERTEMP         (0x10U)

/* ISO 14229-1 DTC status byte of a confirmed, currently failing DTC:
 * testFailed | testFailedThisOperationCycle | confirmedDTC |
 * testFailedSinceLastClear. Bit 4 (testNotCompletedSinceLastClear) is clear:
 * the test has run. */
#define DTC_STATUS_ACTIVE_CONFIRMED  (0x2BU)

/* Re-assert the DTC at most this often while the fault condition holds. */
#define DTC_REASSERT_PERIOD_MS  (100U)

#define MDEG_PER_MS             (180000 / 10000)  /* 18 mdeg/ms = 18 deg/s */
#define VELOCITY_CRPM           (300)             /* 3.00 rpm = 18 deg/s */
#define FRICTION_CNM            (40)              /* 0.40 Nm */
#define LOAD_MDEG_PER_CNM       (500)             /* 0.01 Nm per 0.5 deg */

typedef struct {
    int32_t  position_mdeg;
    int16_t  velocity_crpm;
    int16_t  torque_cnm;
    int16_t  temp_c;
    uint8_t  status;
} joint_state_t;

static joint_state_t s_joint;
static bool          s_started   = false;
static uint32_t      s_t0_ms     = 0U;
static uint32_t      s_last_dtc_ms = 0U;
static bool          s_have_state = false;

static void put_be16(uint8_t *buf, int16_t v)
{
    const uint16_t u = (uint16_t)v;
    buf[0] = (uint8_t)(u >> 8);
    buf[1] = (uint8_t)(u & 0xFFU);
}

static void put_be32(uint8_t *buf, int32_t v)
{
    const uint32_t u = (uint32_t)v;
    buf[0] = (uint8_t)(u >> 24);
    buf[1] = (uint8_t)((u >> 16) & 0xFFU);
    buf[2] = (uint8_t)((u >> 8) & 0xFFU);
    buf[3] = (uint8_t)(u & 0xFFU);
}

static void compute_state(uint32_t t_ms)
{
    const uint32_t phase   = t_ms % JOINT_SIM_SWEEP_PERIOD_MS;
    const uint32_t half    = JOINT_SIM_SWEEP_PERIOD_MS / 2U;
    const bool     forward = (phase < half);
    const int32_t  leg_ms  = (int32_t)(forward ? phase : (phase - half));
    const uint32_t secs    = t_ms / 1000U;
    int32_t        temp;

    if (forward) {
        s_joint.position_mdeg = -JOINT_SIM_SWEEP_AMPL_MDEG + (leg_ms * MDEG_PER_MS);
        s_joint.velocity_crpm = (int16_t)VELOCITY_CRPM;
    } else {
        s_joint.position_mdeg = JOINT_SIM_SWEEP_AMPL_MDEG - (leg_ms * MDEG_PER_MS);
        s_joint.velocity_crpm = (int16_t)(-VELOCITY_CRPM);
    }

    s_joint.torque_cnm = (int16_t)((forward ? FRICTION_CNM : -FRICTION_CNM) +
                                   (s_joint.position_mdeg / LOAD_MDEG_PER_CNM));

    /* Saturate before converting: secs can be as large as 2^32 / 1000. */
    temp = (secs >= (uint32_t)(JOINT_SIM_TEMP_MAX_C - JOINT_SIM_TEMP_START_C))
           ? (int32_t)JOINT_SIM_TEMP_MAX_C
           : (JOINT_SIM_TEMP_START_C + (int32_t)secs);
    s_joint.temp_c = (int16_t)temp;

    s_joint.status = (uint8_t)(STATUS_ENABLED | STATUS_HOME_COMPLETE);
    if (s_joint.temp_c > (int16_t)JOINT_SIM_TEMP_FAULT_C) {
        s_joint.status |= (uint8_t)(STATUS_FAULT_ACTIVE | STATUS_OVERTEMP);
    }
}

void joint_sim_step(uint32_t now_ms)
{
    uint32_t t_ms;

    if (!s_started) {
        s_started     = true;
        s_t0_ms       = now_ms;
        s_last_dtc_ms = now_ms;
    }
    t_ms = now_ms - s_t0_ms;  /* unsigned: correct across the 2^32 wrap */
    compute_state(t_ms);
    s_have_state = true;

    if (((s_joint.status & STATUS_OVERTEMP) != 0U) &&
        ((now_ms - s_last_dtc_ms) >= DTC_REASSERT_PERIOD_MS)) {
        /* Same call sensor_ecu's monitor uses. Refused (and retried at the
         * next period) while ControlDTCSetting is off. */
        s_last_dtc_ms = now_ms;
        (void)dtc_database_set_status(JOINT_SIM_DTC_OVERTEMP,
                                      (uint8_t)DTC_STATUS_ACTIVE_CONFIRMED);
    }
}

/* ---------------------------------------------------------------------------
 * DID readers (declared in generated/did_handlers.h)
 *
 * Before the first joint_sim_step() the joint reads as at t = 0, which keeps
 * a request that races the first loop iteration well defined.
 * ---------------------------------------------------------------------------*/

static void ensure_state(void)
{
    if (!s_have_state) {
        compute_state(0U);
    }
}

uds_status_t did_read_axis0position(uint8_t *buf, uint16_t buf_len,
                                    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < 4U) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
    ensure_state();
    put_be32(buf, s_joint.position_mdeg);
    *out_len = 4U;
    return UDS_STATUS_OK;
}

uds_status_t did_read_axis0periodicposition(uint8_t *buf, uint16_t buf_len,
                                            uint16_t *out_len)
{
    /* 0xF200: the 0x2A-subscribable alias of 0xA000. */
    return did_read_axis0position(buf, buf_len, out_len);
}

uds_status_t did_read_axis0velocity(uint8_t *buf, uint16_t buf_len,
                                    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < 2U) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
    ensure_state();
    put_be16(buf, s_joint.velocity_crpm);
    *out_len = 2U;
    return UDS_STATUS_OK;
}

uds_status_t did_read_axis0torque(uint8_t *buf, uint16_t buf_len,
                                  uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < 2U) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
    ensure_state();
    put_be16(buf, s_joint.torque_cnm);
    *out_len = 2U;
    return UDS_STATUS_OK;
}

uds_status_t did_read_axis0motortemperature(uint8_t *buf, uint16_t buf_len,
                                            uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < 1U) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
    ensure_state();
    buf[0] = (uint8_t)(s_joint.temp_c + 40);
    *out_len = 1U;
    return UDS_STATUS_OK;
}

uds_status_t did_read_axis0statusbitmask(uint8_t *buf, uint16_t buf_len,
                                         uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < 1U) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
    ensure_state();
    buf[0] = s_joint.status;
    *out_len = 1U;
    return UDS_STATUS_OK;
}
