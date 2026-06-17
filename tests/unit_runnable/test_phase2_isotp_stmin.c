// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_phase2_isotp_stmin.c
 *
 * PURPOSE: Phase-2 regression tests — ISO-TP multi-frame TX, STmin inter-frame
 *          delay enforcement, Bs timeout, block-size handling.
 *
 * TEST CASES:
 *   TC-STMIN-001  After FF → TX_WAIT_FC state
 *   TC-STMIN-002  STmin=0: CF sent on first tick after FC CTS
 *   TC-STMIN-003  STmin=5ms: no CF before 5 ticks
 *   TC-STMIN-004  STmin=5ms: CF sent on 5th tick
 *   TC-STMIN-005  BS=2: pauses after 2 CFs, re-enters TX_WAIT_FC
 *   TC-STMIN-006  Bs timeout: no FC in 75ms → ERROR state
 *   TC-STMIN-007  STmin 0xF1 (100µs) decoded as 1ms
 *   TC-STMIN-008  STmin 0x80 (reserved) decoded as 0ms
 *   TC-STMIN-009  FC OVERFLOW → ERROR state
 *   TC-STMIN-010  FC WAIT restarts Bs timer
 *   TC-STMIN-011  Full 13-byte transfer: 1 FF + 2 CF at STmin=0
 *
 * FRAMEWORK: Zephyr Ztest (host shim)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stdbool.h>
#include "isotp.h"
#include "can_transport.h"
#include "uds_types.h"

/* =========================================================================
 * Mock CAN transport
 * ========================================================================= */

#define MAX_TX_FRAMES (64U)

static uds_can_frame_t g_tx_frames[MAX_TX_FRAMES];
static uint8_t         g_tx_count;
static uds_status_t    g_tx_return;

static uds_status_t mock_tx(can_transport_t *self, const uds_can_frame_t *frame)
{
    (void)self;
    if (g_tx_count < MAX_TX_FRAMES) {
        g_tx_frames[g_tx_count++] = *frame;
    }
    return g_tx_return;
}

static uds_status_t mock_rx(can_transport_t *self, uds_can_frame_t *f, bool *ready)
{
    (void)self; (void)f; *ready = false; return UDS_STATUS_OK;
}

static uds_status_t mock_status(can_transport_t *self, bool *bus_off)
{
    (void)self; *bus_off = false; return UDS_STATUS_OK;
}

static const can_transport_ops_t g_ops = {
    .transmit   = mock_tx,
    .receive    = mock_rx,
    .get_status = mock_status,
};

static can_transport_t g_can = {
    .ops     = &g_ops,
    .platform = NULL,
    .ready   = true,
};

static void mock_reset(void)
{
    memset(g_tx_frames, 0, sizeof(g_tx_frames));
    g_tx_count  = 0U;
    g_tx_return = UDS_STATUS_OK;
}

/* Null RX callback — used for TX-only tests */
static void null_rx_cb(const uint8_t *d, uint32_t l, void *a)
{
    (void)d; (void)l; (void)a;
}

static uds_can_frame_t make_fc(uint8_t fs, uint8_t bs, uint8_t stmin)
{
    uds_can_frame_t f; memset(&f, 0, sizeof(f));
    f.id = 0x7DFU; f.dlc = 3U;
    f.data[0] = (uint8_t)(0x30U | (fs & 0x0FU));
    f.data[1] = bs; f.data[2] = stmin;
    return f;
}

static isotp_ctx_t make_ctx(void)
{
    isotp_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    isotp_cfg_t cfg = {
        .rx_can_id = 0x7DFU, .tx_can_id = 0x7E8U,
        .block_size = 0U, .stmin_ms = 0U, .can = &g_can,
    };
    isotp_init(&ctx, &cfg);
    return ctx;
}

/* =========================================================================
 * Tests
 * ========================================================================= */

ZTEST_SUITE(test_phase2_isotp_stmin, NULL, NULL, NULL, NULL, NULL);

ZTEST(test_phase2_isotp_stmin, tc001_ff_sets_wait_fc)
{
    mock_reset();
    isotp_ctx_t ctx = make_ctx();
    uint8_t data[10] = {1,2,3,4,5,6,7,8,9,10};
    zassert_equal(UDS_STATUS_OK, isotp_transmit(&ctx, data, 10U), "");
    zassert_equal(ISOTP_STATE_TX_WAIT_FC, ctx.tx_state, "State must be TX_WAIT_FC");
    zassert_equal(1U, g_tx_count, "Exactly 1 FF transmitted");
}

ZTEST(test_phase2_isotp_stmin, tc002_stmin0_cf_on_first_tick)
{
    mock_reset();
    isotp_ctx_t ctx = make_ctx();
    uint8_t data[10] = {1,2,3,4,5,6,7,8,9,10};
    isotp_transmit(&ctx, data, 10U);
    uint8_t ff_count = g_tx_count;

    /* Simulate FC CTS via state injection */
    ctx.tx_block_size     = 0U;
    ctx.tx_stmin_ms       = 0U;
    ctx.tx_stmin_timer_ms = 0U;
    ctx.tx_bs_timer_ms    = 0U;
    ctx.tx_state          = ISOTP_STATE_TX_SEND_CF;

    isotp_tick_1ms(&ctx);
    zassert_equal((uint8_t)(ff_count + 1U), g_tx_count, "CF must be sent on first tick");
}

ZTEST(test_phase2_isotp_stmin, tc003_stmin5_no_cf_before_5_ticks)
{
    mock_reset();
    isotp_ctx_t ctx = make_ctx();
    uint8_t data[10] = {1,2,3,4,5,6,7,8,9,10};
    isotp_transmit(&ctx, data, 10U);
    uint8_t ff_count = g_tx_count;

    ctx.tx_stmin_ms = 5U; ctx.tx_stmin_timer_ms = 5U;
    ctx.tx_bs_timer_ms = 0U; ctx.tx_block_size = 0U;
    ctx.tx_state = ISOTP_STATE_TX_SEND_CF;

    for (int i = 0; i < 4; i++) { isotp_tick_1ms(&ctx); }
    zassert_equal(ff_count, g_tx_count, "No CF before 5 ticks");
}

ZTEST(test_phase2_isotp_stmin, tc004_stmin5_cf_on_5th_tick)
{
    mock_reset();
    isotp_ctx_t ctx = make_ctx();
    uint8_t data[10] = {1,2,3,4,5,6,7,8,9,10};
    isotp_transmit(&ctx, data, 10U);
    uint8_t ff_count = g_tx_count;

    ctx.tx_stmin_ms = 5U; ctx.tx_stmin_timer_ms = 5U;
    ctx.tx_bs_timer_ms = 0U; ctx.tx_block_size = 0U;
    ctx.tx_state = ISOTP_STATE_TX_SEND_CF;

    for (int i = 0; i < 5; i++) { isotp_tick_1ms(&ctx); }
    zassert_equal((uint8_t)(ff_count + 1U), g_tx_count, "CF sent on 5th tick");
}

ZTEST(test_phase2_isotp_stmin, tc005_block_size_2_pauses)
{
    mock_reset();
    isotp_ctx_t ctx = make_ctx();
    /* 30 bytes: FF uses 6, leaving 24 bytes = 4 CFs of 7,7,7,3.
     * With BS=2 the pump stops after CF1+CF2 and re-enters TX_WAIT_FC. */
    uint8_t data[30];
    for (uint8_t i = 0; i < 30U; i++) { data[i] = i; }
    isotp_transmit(&ctx, data, 30U);

    ctx.tx_block_size = 2U; ctx.tx_stmin_ms = 0U;
    ctx.tx_stmin_timer_ms = 0U; ctx.tx_bs_timer_ms = 0U;
    ctx.tx_blocks_sent = 0U; ctx.tx_state = ISOTP_STATE_TX_SEND_CF;

    for (int i = 0; i < 3; i++) { isotp_tick_1ms(&ctx); }
    zassert_equal(ISOTP_STATE_TX_WAIT_FC, ctx.tx_state,
                  "Must pause and wait for FC after BS=2 CFs");
}

ZTEST(test_phase2_isotp_stmin, tc006_bs_timeout)
{
    mock_reset();
    isotp_ctx_t ctx = make_ctx();
    uint8_t data[10] = {1,2,3,4,5,6,7,8,9,10};
    isotp_transmit(&ctx, data, 10U); /* arms Bs timer */

    /* Disable As timer so Bs fires first (Bs=75ms > As=25ms otherwise). */
    ctx.tx_as_timer_ms = 0U;

    uds_status_t rc = UDS_STATUS_OK;
    for (int i = 0; i <= (int)ISOTP_TIMEOUT_BS_MS + 2; i++) {
        rc = isotp_tick_1ms(&ctx);
        if (rc != UDS_STATUS_OK) break;
    }
    zassert_equal(UDS_STATUS_ERR_TP_TIMEOUT_BS, rc, "Bs timeout must fire");
    zassert_equal(ISOTP_STATE_ERROR, ctx.tx_state, "State must be ERROR");
}

ZTEST(test_phase2_isotp_stmin, tc007_stmin_sub_ms_to_1ms)
{
    mock_reset();
    isotp_ctx_t ctx = make_ctx();
    uint8_t data[10] = {1,2,3,4,5,6,7,8,9,10};
    isotp_transmit(&ctx, data, 10U);

    ctx.tx_state = ISOTP_STATE_TX_WAIT_FC;
    ctx.tx_bs_timer_ms = ISOTP_TIMEOUT_BS_MS;
    uds_can_frame_t fc = make_fc(0x00U, 0U, 0xF1U); /* 100µs */
    isotp_process_rx_frame(&ctx, &fc, null_rx_cb, NULL);

    zassert_equal(1U, ctx.tx_stmin_ms, "0xF1 must decode as 1ms");
}

ZTEST(test_phase2_isotp_stmin, tc008_stmin_reserved_to_0ms)
{
    mock_reset();
    isotp_ctx_t ctx = make_ctx();
    uint8_t data[10] = {1,2,3,4,5,6,7,8,9,10};
    isotp_transmit(&ctx, data, 10U);

    ctx.tx_state = ISOTP_STATE_TX_WAIT_FC;
    ctx.tx_bs_timer_ms = ISOTP_TIMEOUT_BS_MS;
    uds_can_frame_t fc = make_fc(0x00U, 0U, 0x80U); /* reserved */
    isotp_process_rx_frame(&ctx, &fc, null_rx_cb, NULL);

    zassert_equal(0U, ctx.tx_stmin_ms, "0x80 must decode as 0ms");
}

ZTEST(test_phase2_isotp_stmin, tc009_fc_overflow_sets_error)
{
    mock_reset();
    isotp_ctx_t ctx = make_ctx();
    uint8_t data[10] = {1,2,3,4,5,6,7,8,9,10};
    isotp_transmit(&ctx, data, 10U);

    ctx.tx_bs_timer_ms = ISOTP_TIMEOUT_BS_MS;
    uds_can_frame_t fc = make_fc(0x02U, 0U, 0U); /* OVFLW */
    uds_status_t rc = isotp_process_rx_frame(&ctx, &fc, null_rx_cb, NULL);

    zassert_equal(UDS_STATUS_ERR_TP_OVERFLOW, rc, "OVERFLOW FC must return ERR");
    zassert_equal(ISOTP_STATE_ERROR, ctx.tx_state, "State must be ERROR");
}

ZTEST(test_phase2_isotp_stmin, tc010_fc_wait_restarts_bs)
{
    mock_reset();
    isotp_ctx_t ctx = make_ctx();
    uint8_t data[10] = {1,2,3,4,5,6,7,8,9,10};
    isotp_transmit(&ctx, data, 10U);

    ctx.tx_bs_timer_ms = 10U; /* partially drained */
    uds_can_frame_t fc = make_fc(0x01U, 0U, 0U); /* WAIT */
    isotp_process_rx_frame(&ctx, &fc, null_rx_cb, NULL);

    zassert_equal((uint32_t)ISOTP_TIMEOUT_BS_MS, ctx.tx_bs_timer_ms,
                  "FC WAIT must restart Bs timer");
}

ZTEST(test_phase2_isotp_stmin, tc011_full_20_byte_transfer)
{
    mock_reset();
    isotp_ctx_t ctx = make_ctx();
    /* 20 bytes: FF carries 6, CF1 carries 7, CF2 carries 7 = 20 total.
     * Expected: 1 FF + 2 CFs = 3 frames. */
    uint8_t data[20];
    for (uint8_t i = 0; i < 20U; i++) { data[i] = i; }
    isotp_transmit(&ctx, data, 20U); /* sends FF */

    ctx.tx_block_size = 0U; ctx.tx_stmin_ms = 0U;
    ctx.tx_stmin_timer_ms = 0U; ctx.tx_bs_timer_ms = 0U;
    ctx.tx_state = ISOTP_STATE_TX_SEND_CF;

    for (int i = 0; i < 5; i++) {
        isotp_tick_1ms(&ctx);
        if (ctx.tx_state == ISOTP_STATE_IDLE) break;
    }
    zassert_equal(3U, g_tx_count, "FF + 2 CFs = 3 total frames");
    zassert_equal(ISOTP_STATE_IDLE, ctx.tx_state, "State must be IDLE");
}

extern void test_phase2_isotp_stmin__tc001_ff_sets_wait_fc(void);
extern void test_phase2_isotp_stmin__tc002_stmin0_cf_on_first_tick(void);
extern void test_phase2_isotp_stmin__tc003_stmin5_no_cf_before_5_ticks(void);
extern void test_phase2_isotp_stmin__tc004_stmin5_cf_on_5th_tick(void);
extern void test_phase2_isotp_stmin__tc005_block_size_2_pauses(void);
extern void test_phase2_isotp_stmin__tc006_bs_timeout(void);
extern void test_phase2_isotp_stmin__tc007_stmin_sub_ms_to_1ms(void);
extern void test_phase2_isotp_stmin__tc008_stmin_reserved_to_0ms(void);
extern void test_phase2_isotp_stmin__tc009_fc_overflow_sets_error(void);
extern void test_phase2_isotp_stmin__tc010_fc_wait_restarts_bs(void);
extern void test_phase2_isotp_stmin__tc011_full_20_byte_transfer(void);

void run_all_tests(void)
{
    RUN_TEST(test_phase2_isotp_stmin__tc001_ff_sets_wait_fc);
    RUN_TEST(test_phase2_isotp_stmin__tc002_stmin0_cf_on_first_tick);
    RUN_TEST(test_phase2_isotp_stmin__tc003_stmin5_no_cf_before_5_ticks);
    RUN_TEST(test_phase2_isotp_stmin__tc004_stmin5_cf_on_5th_tick);
    RUN_TEST(test_phase2_isotp_stmin__tc005_block_size_2_pauses);
    RUN_TEST(test_phase2_isotp_stmin__tc006_bs_timeout);
    RUN_TEST(test_phase2_isotp_stmin__tc007_stmin_sub_ms_to_1ms);
    RUN_TEST(test_phase2_isotp_stmin__tc008_stmin_reserved_to_0ms);
    RUN_TEST(test_phase2_isotp_stmin__tc009_fc_overflow_sets_error);
    RUN_TEST(test_phase2_isotp_stmin__tc010_fc_wait_restarts_bs);
    RUN_TEST(test_phase2_isotp_stmin__tc011_full_20_byte_transfer);
}
