// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/bms_ecu/src/bms_sim.c
 *
 * PURPOSE: See bms_sim.h. Encodings follow diagnostics_config.yaml; every value
 *          is big-endian, temperatures are offset by +40 degC, SoC/SoH are
 *          0.4 %/LSB.
 * =============================================================================
 */

#include "bms_sim.h"

#include <stddef.h>
#include <stdint.h>

#include "uds_types.h"
#include "did_handlers.h"

/* Boot values */
#define BMS_PACK_VOLTAGE_MV      (357120UL)  /* 8 groups x 12 cells x 3.72 V    */
#define BMS_CELL_GROUP_MV        (44640U)    /* 12 cells x 3.72 V               */
#define BMS_TEMP_25C_RAW         (65U)       /* 25 degC + 40                    */
#define BMS_SOC_50PCT_RAW        (125U)      /* 0.4 %/LSB                       */
#define BMS_SOH_100PCT_RAW       (250U)
#define BMS_FULL_CAPACITY_WH     (75000UL)
#define BMS_OV_THRESHOLD_MV      (4200U)
#define BMS_UV_THRESHOLD_MV      (2800U)

/* Write ranges for the NMC thresholds (documented in the YAML) */
#define BMS_OV_MIN_MV            (3800U)
#define BMS_OV_MAX_MV            (4350U)
#define BMS_UV_MIN_MV            (2500U)
#define BMS_UV_MAX_MV            (3200U)

static struct {
    uint32_t pack_voltage_mV;
    int16_t  pack_current_100mA;
    int16_t  pack_power_10W;
    uint8_t  contactor_state;
    uint16_t isolation_resistance_kOhm;
    uint16_t cell_group_mV[4U];
    uint8_t  temp_cell_max_raw;
    uint8_t  temp_cell_min_raw;
    uint8_t  temp_coolant_inlet_raw;
    uint8_t  temp_bms_board_raw;
    uint8_t  soc_raw;
    uint8_t  soh_raw;
    uint8_t  balancing_state;
    uint16_t fault_flags;
    uint32_t full_charge_capacity_Wh;
    uint16_t ov_threshold_mV;
    uint16_t uv_threshold_mV;
} s_bms;

void bms_sim_init(void)
{
    uint8_t i;

    s_bms.pack_voltage_mV           = BMS_PACK_VOLTAGE_MV;
    s_bms.pack_current_100mA        = 0;          /* idle                       */
    s_bms.pack_power_10W            = 0;
    s_bms.contactor_state           = 0x00U;      /* all open                   */
    s_bms.isolation_resistance_kOhm = 0xFFFFU;    /* IMD not started            */
    for (i = 0U; i < 4U; i++) {
        s_bms.cell_group_mV[i] = BMS_CELL_GROUP_MV;
    }
    s_bms.temp_cell_max_raw         = BMS_TEMP_25C_RAW;
    s_bms.temp_cell_min_raw         = BMS_TEMP_25C_RAW;
    s_bms.temp_coolant_inlet_raw    = BMS_TEMP_25C_RAW;
    s_bms.temp_bms_board_raw        = BMS_TEMP_25C_RAW;
    s_bms.soc_raw                   = BMS_SOC_50PCT_RAW;
    s_bms.soh_raw                   = BMS_SOH_100PCT_RAW;
    s_bms.balancing_state           = 0x00U;
    s_bms.fault_flags               = 0x0000U;
    s_bms.full_charge_capacity_Wh   = BMS_FULL_CAPACITY_WH;
    s_bms.ov_threshold_mV           = BMS_OV_THRESHOLD_MV;
    s_bms.uv_threshold_mV           = BMS_UV_THRESHOLD_MV;
}

/* ---------------------------------------------------------------------------
 * Encoding helpers: validate the call, then write n big-endian bytes.
 * --------------------------------------------------------------------------- */

static uds_status_t put(uint8_t *buf, uint16_t buf_len, uint16_t *out_len,
                        uint32_t v, uint16_t n)
{
    uint16_t i;

    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < n) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
    for (i = 0U; i < n; i++) {
        buf[i] = (uint8_t)((v >> (8U * (uint32_t)(n - 1U - i))) & 0xFFU);
    }
    *out_len = n;
    return UDS_STATUS_OK;
}

/* ---------------------------------------------------------------------------
 * Pack (0xDB00-0xDB04)
 * --------------------------------------------------------------------------- */

uds_status_t did_read_bms_packvoltage_mv(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.pack_voltage_mV, 4U); }

uds_status_t did_read_bms_packcurrent_100ma(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, (uint32_t)(uint16_t)s_bms.pack_current_100mA, 2U); }

uds_status_t did_read_bms_packpower_10w(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, (uint32_t)(uint16_t)s_bms.pack_power_10W, 2U); }

uds_status_t did_read_bms_contactorstate(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.contactor_state, 1U); }

uds_status_t did_read_bms_insulationresistance_kohm(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.isolation_resistance_kOhm, 2U); }

/* ---------------------------------------------------------------------------
 * Cell groups (0xDC01-0xDC04)
 * --------------------------------------------------------------------------- */

uds_status_t did_read_bms_cellgroup1_voltage_mv(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.cell_group_mV[0U], 2U); }

uds_status_t did_read_bms_cellgroup2_voltage_mv(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.cell_group_mV[1U], 2U); }

uds_status_t did_read_bms_cellgroup3_voltage_mv(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.cell_group_mV[2U], 2U); }

uds_status_t did_read_bms_cellgroup4_voltage_mv(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.cell_group_mV[3U], 2U); }

/* ---------------------------------------------------------------------------
 * Temperatures (0xDD00-0xDD03), State (0xDE00-0xDE02)
 * --------------------------------------------------------------------------- */

uds_status_t did_read_bms_maxcelltemperature_degc(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.temp_cell_max_raw, 1U); }

uds_status_t did_read_bms_mincelltemperature_degc(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.temp_cell_min_raw, 1U); }

uds_status_t did_read_bms_coolantinlettemperature_degc(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.temp_coolant_inlet_raw, 1U); }

uds_status_t did_read_bms_bmsboardtemperature_degc(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.temp_bms_board_raw, 1U); }

uds_status_t did_read_bms_stateofcharge_04pct(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.soc_raw, 1U); }

uds_status_t did_read_bms_stateofhealth_04pct(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.soh_raw, 1U); }

uds_status_t did_read_bms_balancingstatebitmask(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.balancing_state, 1U); }

/* ---------------------------------------------------------------------------
 * System health (0xD900, 0xD901) and thresholds (0xD910, 0xD911, read/write)
 * --------------------------------------------------------------------------- */

uds_status_t did_read_bms_faultflagsbitmask(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.fault_flags, 2U); }

uds_status_t did_read_bms_fullchargecapacity_wh(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.full_charge_capacity_Wh, 4U); }

uds_status_t did_read_bms_ovthreshold_mv(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.ov_threshold_mV, 2U); }

uds_status_t did_read_bms_uvthreshold_mv(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{ return put(buf, buf_len, out_len, s_bms.uv_threshold_mV, 2U); }

static uds_status_t write_threshold(const uint8_t *buf, uint16_t len,
                                    uint16_t min_mv, uint16_t max_mv,
                                    uint16_t *dst)
{
    uint16_t t;

    if (buf == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    if (len != 2U) { return UDS_STATUS_ERR_INVALID_PARAM; }
    t = (uint16_t)(((uint16_t)buf[0U] << 8U) | (uint16_t)buf[1U]);
    if ((t < min_mv) || (t > max_mv)) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }
    *dst = t;
    return UDS_STATUS_OK;
}

uds_status_t did_write_bms_ovthreshold_mv(const uint8_t *buf, uint16_t len)
{ return write_threshold(buf, len, BMS_OV_MIN_MV, BMS_OV_MAX_MV, &s_bms.ov_threshold_mV); }

uds_status_t did_write_bms_uvthreshold_mv(const uint8_t *buf, uint16_t len)
{ return write_threshold(buf, len, BMS_UV_MIN_MV, BMS_UV_MAX_MV, &s_bms.uv_threshold_mV); }
