// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
// ================================
// File: tests/unit/test_isotp.c
// ================================
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit/test_isotp.c
 *
 * MODULE UNDER TEST: transport/isotp.c
 *
 * PURPOSE:
 *   Verify ISO 15765-2 (ISO-TP) transport layer logic. Tests cover:
 *     - isotp_init: happy path, NULL guards, already-initialised guard,
 *       invalid CAN IDs
 *     - isotp_process_rx_frame: Single Frame (SF) reassembly,
 *       First Frame (FF) + Consecutive Frame (CF) multi-frame reassembly,
 *       Flow Control (FC) frame handling, RX buffer overflow, unknown PCI
 *     - isotp_transmit: single-frame path, multi-frame initiation,
 *       busy guard, NULL/zero-length/overflow guards
 *     - isotp_tick_1ms: Cr timeout detection
 *     - isotp_reset: clears state back to IDLE
 *     - isotp_get_state: NULL guard, state reporting
 *
 * DID constant cross-references:
 *   0x0C00 Engine Speed  → 2 bytes  (single frame)
 *   0xF190 VIN           → 17 bytes (multi-frame)
 *
 * FRAMEWORK: Zephyr Ztest
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "isotp.h"
#include "can_transport.h"
#include "uds_types.h"

/* =========================================================================
 * Mock CAN transport
 * ========================================================================= */

static uds_can_frame_t  g_mock_tx_frames[16];
static uint8_t          g_mock_tx_count;
static uds_status_t     g_mock_tx_return;

/*
 * [#122] Flow-Control-specific transmit failure injection.
 *
 * g_mock_tx_return fails every frame indiscriminately, which cannot separate
 * a rejected FC from a rejected SF/FF/CF. These hooks reject ONLY frames whose
 * N_PCItype is FC, so the receiver-side FC transmit path can be exercised in
 * isolation.
 *
 * g_mock_observed_ctx additionally samples ctx->rx_ar_timer_ms from *inside*
 * the transmit call — i.e. within the N_Ar confirmation window — which is the
 * only point at which an armed N_Ar is observable.
 */
static bool               g_mock_fail_on_fc;
static const isotp_ctx_t *g_mock_observed_ctx;
static uint32_t           g_mock_ar_timer_during_fc;
static bool               g_mock_fc_observed;

static uds_status_t mock_can_transmit(can_transport_t *self, const uds_can_frame_t *frame)
{
    (void)self;
    if (g_mock_tx_count < 16U) {
        g_mock_tx_frames[g_mock_tx_count++] = *frame;
    }

    if ((uint8_t)((frame->data[0] >> 4U) & 0x0FU) == (uint8_t)ISOTP_FRAME_TYPE_FC) {
        if (g_mock_observed_ctx != NULL) {
            g_mock_ar_timer_during_fc = g_mock_observed_ctx->rx_ar_timer_ms;
            g_mock_fc_observed        = true;
        }
        if (g_mock_fail_on_fc) {
            return UDS_STATUS_ERR_CAN_TX_FAILED;
        }
    }

    return g_mock_tx_return;
}

static uds_status_t mock_can_receive(can_transport_t *self,
                                     uds_can_frame_t *out_frame,
                                     bool            *out_ready)
{
    (void)self; (void)out_frame;
    *out_ready = false;
    return UDS_STATUS_OK;
}

static uds_status_t mock_can_status(can_transport_t *self, bool *bus_off)
{
    (void)self;
    *bus_off = false;
    return UDS_STATUS_OK;
}

static const can_transport_ops_t g_mock_can_ops = {
    .transmit   = mock_can_transmit,
    .receive    = mock_can_receive,
    .get_status = mock_can_status,
};

static can_transport_t g_mock_can = {
    .ops      = &g_mock_can_ops,
    .platform = NULL,
    .ready    = true,
};

/* RX callback state */
static uint8_t  g_rx_cb_data[UDS_MAX_PAYLOAD_LEN];
static uint32_t g_rx_cb_len;
static bool     g_rx_cb_called;

static void rx_complete_cb(const uint8_t *data, uint32_t length, void *arg)
{
    (void)arg;
    g_rx_cb_called = true;
    g_rx_cb_len    = length;
    if (length <= (uint32_t)UDS_MAX_PAYLOAD_LEN) {
        memcpy(g_rx_cb_data, data, (size_t)length);
    }
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

static void mock_can_reset(void)
{
    memset(g_mock_tx_frames, 0, sizeof(g_mock_tx_frames));
    g_mock_tx_count  = 0U;
    g_mock_tx_return = UDS_STATUS_OK;
    /* [#122] */
    g_mock_fail_on_fc         = false;
    g_mock_observed_ctx       = NULL;
    g_mock_ar_timer_during_fc = 0U;
    g_mock_fc_observed        = false;
}

static void rx_cb_reset(void)
{
    memset(g_rx_cb_data, 0, sizeof(g_rx_cb_data));
    g_rx_cb_len    = 0U;
    g_rx_cb_called = false;
}

/** Build a Classic CAN frame (helper). */
static uds_can_frame_t make_can_frame(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    uds_can_frame_t f;
    memset(&f, 0, sizeof(f));
    f.id  = id;
    f.dlc = dlc;
    if (data != NULL) {
        memcpy(f.data, data, dlc);
    }
    return f;
}

#if ISOTP_ENABLE_CAN_FD
/** Build a CAN FD frame (helper). */
static uds_can_frame_t make_fd_frame(uint32_t id, const uint8_t *data, uint8_t dlc)
{
    uds_can_frame_t f = make_can_frame(id, data, dlc);
    f.is_fd = true;
    return f;
}
#endif /* ISOTP_ENABLE_CAN_FD */

/** Initialise a fresh isotp_ctx_t with the mock CAN transport (Classic CAN). */
static uds_status_t init_isotp(isotp_ctx_t *ctx)
{
    isotp_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rx_can_id   = 0x7DFU;
    cfg.tx_can_id   = 0x7E8U;
    cfg.block_size  = 0U;
    cfg.stmin_ms    = 0U;
#if ISOTP_ENABLE_CAN_FD
    cfg.use_fd      = false;
#endif
    cfg.can         = &g_mock_can;
    return isotp_init(ctx, &cfg);
}

/**
 * Initialise a fresh isotp_ctx_t advertising a non-zero BlockSize / STmin
 * (Classic CAN).  [#121] Used by the RX block-size regression tests.
 */
static uds_status_t init_isotp_bs(isotp_ctx_t *ctx, uint8_t block_size, uint8_t stmin_ms)
{
    isotp_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rx_can_id   = 0x7DFU;
    cfg.tx_can_id   = 0x7E8U;
    cfg.block_size  = block_size;
    cfg.stmin_ms    = stmin_ms;
#if ISOTP_ENABLE_CAN_FD
    cfg.use_fd      = false;
#endif
    cfg.can         = &g_mock_can;
    return isotp_init(ctx, &cfg);
}

#if ISOTP_ENABLE_CAN_FD
/** Initialise a fresh isotp_ctx_t in CAN FD mode. */
static uds_status_t init_isotp_fd(isotp_ctx_t *ctx)
{
    isotp_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rx_can_id   = 0x7DFU;
    cfg.tx_can_id   = 0x7E8U;
    cfg.block_size  = 0U;
    cfg.stmin_ms    = 0U;
    cfg.use_fd      = true;
    cfg.can         = &g_mock_can;
    return isotp_init(ctx, &cfg);
}
#endif /* ISOTP_ENABLE_CAN_FD */

/* =========================================================================
 * Test suite: isotp_init
 * ========================================================================= */

ZTEST_SUITE(test_isotp_init, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-ISTP-INIT-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_isotp_init, test_null_ctx)
{
    isotp_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rx_can_id = 0x7DFU;
    cfg.tx_can_id = 0x7E8U;
    cfg.can       = &g_mock_can;
    uds_status_t rc = isotp_init(NULL, &cfg);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "Expected NULL_PTR for NULL ctx");
}

/**
 * TC-ISTP-INIT-002: NULL cfg → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_isotp_init, test_null_cfg)
{
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    uds_status_t rc = isotp_init(&ctx, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "Expected NULL_PTR for NULL cfg");
}

/**
 * TC-ISTP-INIT-003: NULL CAN transport in cfg → UDS_STATUS_ERR_INVALID_PARAM.
 */
ZTEST(test_isotp_init, test_null_can)
{
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    isotp_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rx_can_id = 0x7DFU;
    cfg.tx_can_id = 0x7E8U;
    cfg.can       = NULL;
    uds_status_t rc = isotp_init(&ctx, &cfg);
    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "Expected INVALID_PARAM for NULL CAN");
}

/**
 * TC-ISTP-INIT-004: Valid configuration → UDS_STATUS_OK, state = IDLE.
 */
ZTEST(test_isotp_init, test_happy_path)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    uds_status_t rc = init_isotp(&ctx);
    zassert_equal(rc, UDS_STATUS_OK, "Expected OK for valid init");
    zassert_true(ctx.initialized, "ctx.initialized must be set");

    isotp_state_t state;
    rc = isotp_get_state(&ctx, &state);
    zassert_equal(rc, UDS_STATUS_OK, "get_state must succeed after init");
    zassert_equal(state, ISOTP_STATE_IDLE, "Expected IDLE after init");
}

/**
 * TC-ISTP-INIT-005: Double init → UDS_STATUS_ERR_ALREADY_INITIALIZED.
 */
ZTEST(test_isotp_init, test_double_init)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "First init must succeed");
    uds_status_t rc = init_isotp(&ctx);
    zassert_equal(rc, UDS_STATUS_ERR_ALREADY_INITIALIZED,
                  "Second init must return ALREADY_INITIALIZED");
}

/* =========================================================================
 * Test suite: isotp_process_rx_frame — Single Frame path
 * ========================================================================= */

ZTEST_SUITE(test_isotp_rx_single, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-ISTP-RX-SF-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_isotp_rx_single, test_null_ctx)
{
    uds_can_frame_t f = {0};
    uds_status_t rc = isotp_process_rx_frame(NULL, &f, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must fail");
}

/**
 * TC-ISTP-RX-SF-002: NULL frame → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_isotp_rx_single, test_null_frame)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");
    uds_status_t rc = isotp_process_rx_frame(&ctx, NULL, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL frame must fail");
}

/**
 * TC-ISTP-RX-SF-003: Valid Single Frame (7-byte payload) → callback fires.
 *
 * UDS ReadDataByIdentifier request: [0x22, 0xF1, 0x90] = 3 bytes.
 * ISO-TP SF encoding: data[0] = 0x03 (SF, len=3), data[1..3] = payload.
 */
ZTEST(test_isotp_rx_single, test_single_frame_3_bytes)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    /* Build SF: PCI byte 0x03 = SF with data_len=3 */
    uint8_t payload[] = { 0x03U, 0x22U, 0xF1U, 0x90U, 0x00U, 0x00U, 0x00U, 0x00U };
    uds_can_frame_t f = make_can_frame(0x7DFU, payload, 8U);

    uds_status_t rc = isotp_process_rx_frame(&ctx, &f, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "SF processing must return OK");
    zassert_true(g_rx_cb_called, "RX callback must be fired for complete SF");
    zassert_equal(g_rx_cb_len, 3U, "Reassembled length must be 3");
    zassert_equal(g_rx_cb_data[0], 0x22U, "Byte 0 mismatch (SID)");
    zassert_equal(g_rx_cb_data[1], 0xF1U, "Byte 1 mismatch (DID hi)");
    zassert_equal(g_rx_cb_data[2], 0x90U, "Byte 2 mismatch (DID lo)");
}

/**
 * TC-ISTP-RX-SF-004: Single Frame with data_len=0 → UDS_STATUS_ERR_TP_FRAME_INVALID.
 * ISO 15765-2: SF with SFdl=0 is invalid.
 */
ZTEST(test_isotp_rx_single, test_sf_zero_length)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t payload[] = { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
    uds_can_frame_t f = make_can_frame(0x7DFU, payload, 8U);
    uds_status_t rc = isotp_process_rx_frame(&ctx, &f, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_TP_FRAME_INVALID,
                  "SF with len=0 must be rejected as invalid");
    zassert_false(g_rx_cb_called, "Callback must not fire for invalid SF");
}

/**
 * TC-ISTP-RX-SF-005: Single Frame, maximum classic CAN payload (7 data bytes).
 * Validates DID 0x22 request for VIN — 3 bytes, still single-frame.
 */
ZTEST(test_isotp_rx_single, test_sf_seven_bytes)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    /* 7-byte payload (maximum single-frame for classic CAN) */
    uint8_t payload[] = { 0x07U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU, 0x11U };
    uds_can_frame_t f = make_can_frame(0x7DFU, payload, 8U);

    uds_status_t rc = isotp_process_rx_frame(&ctx, &f, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "7-byte SF must succeed");
    zassert_true(g_rx_cb_called, "Callback must fire");
    zassert_equal(g_rx_cb_len, 7U, "Length must be 7");
    zassert_equal(g_rx_cb_data[0], 0xAAU, "data[0] mismatch");
    zassert_equal(g_rx_cb_data[6], 0x11U, "data[6] mismatch");
}

/* =========================================================================
 * Test suite: isotp_process_rx_frame — Multi-Frame path
 * ========================================================================= */

ZTEST_SUITE(test_isotp_rx_multi, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-ISTP-RX-MF-001: First Frame → state transitions to RX_WAIT_CF,
 *                     FC frame is transmitted.
 *
 * Simulates reading DID 0xF190 (VIN, 17 bytes).
 * FF encoding: data[0] = 0x10, data[1] = 0x11 (17 bytes total),
 *              data[2..7] = first 6 bytes of payload.
 */
ZTEST(test_isotp_rx_multi, test_first_frame_triggers_fc)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    /* FF: PCI = 0x10 (hi nibble=FF, lo nibble=0), len_lo = 0x11 (17) */
    uint8_t payload[] = { 0x10U, 0x11U, 0x56U, 0x49U, 0x4EU, 0x31U, 0x32U, 0x33U };
    uds_can_frame_t f = make_can_frame(0x7DFU, payload, 8U);

    uds_status_t rc = isotp_process_rx_frame(&ctx, &f, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "FF must be accepted");
    zassert_false(g_rx_cb_called, "Callback must NOT fire after FF alone");

    isotp_state_t state;
    isotp_get_state(&ctx, &state);
    zassert_equal(state, ISOTP_STATE_RX_WAIT_CF,
                  "State must be RX_WAIT_CF after FF");

    /*
     * The implementation should transmit a Flow Control (FC) frame.
     * PCI byte of FC: upper nibble = 3 (FC), lower nibble = 0 (CTS).
     */
    /* Note: FC transmission depends on Phase-2 TODO in isotp.c.
     * If FC TX is stubbed, we simply verify no crash occurred. */
}

/**
 * TC-ISTP-RX-MF-002: FF + CF sequence → callback fires with full payload.
 *
 * Total: 10 bytes, FF carries 6, CF carries 4.
 */
ZTEST(test_isotp_rx_multi, test_ff_cf_complete)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    /* FF: total_len = 10 bytes, first 6 bytes = 0x01..0x06 */
    uint8_t ff_payload[] = { 0x10U, 0x0AU, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U };
    uds_can_frame_t ff = make_can_frame(0x7DFU, ff_payload, 8U);
    zassert_equal(isotp_process_rx_frame(&ctx, &ff, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "FF processing failed");

    /* CF1: SN=1, next 4 bytes = 0x07..0x0A */
    uint8_t cf1_payload[] = { 0x21U, 0x07U, 0x08U, 0x09U, 0x0AU, 0x00U, 0x00U, 0x00U };
    uds_can_frame_t cf1 = make_can_frame(0x7DFU, cf1_payload, 8U);
    zassert_equal(isotp_process_rx_frame(&ctx, &cf1, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "CF1 processing failed");

    /* Callback must fire after final CF */
    zassert_true(g_rx_cb_called, "Callback must fire after complete multi-frame");
    zassert_equal(g_rx_cb_len, 10U, "Reassembled length must be 10");
    zassert_equal(g_rx_cb_data[0], 0x01U, "data[0] mismatch");
    zassert_equal(g_rx_cb_data[5], 0x06U, "data[5] mismatch");
    zassert_equal(g_rx_cb_data[6], 0x07U, "data[6] mismatch");
    zassert_equal(g_rx_cb_data[9], 0x0AU, "data[9] mismatch");
}

/**
 * TC-ISTP-RX-MF-003: CF in IDLE state → UDS_STATUS_ERR_TP_UNEXPECTED_PDU.
 * A CF with no prior FF is an error per ISO 15765-2.
 */
ZTEST(test_isotp_rx_multi, test_cf_without_ff)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t cf_payload[] = { 0x21U, 0x01U, 0x02U, 0x03U, 0x00U, 0x00U, 0x00U, 0x00U };
    uds_can_frame_t cf = make_can_frame(0x7DFU, cf_payload, 8U);
    uds_status_t rc = isotp_process_rx_frame(&ctx, &cf, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_TP_UNEXPECTED_PDU,
                  "CF in IDLE state must be rejected");
    zassert_false(g_rx_cb_called, "Callback must not fire for unexpected CF");
}

/*
 * Shared fixture for the block-size regression tests below.
 *
 * A 37-byte inbound PDU: the FF carries 6 bytes, CF1..CF4 carry 7 bytes each
 * and CF5 carries the trailing 3 bytes.  Five CFs is the smallest transfer
 * that exercises TWO complete blocks at BS = 2 and therefore proves the
 * per-block counter is RESET, not merely fired once.
 */
#define ISOTP_BS_TEST_PDU_LEN  (37U)

/** Fill @p out with the deterministic 37-byte reference payload. */
static void bs_test_make_payload(uint8_t *out)
{
    uint8_t i;
    for (i = 0U; i < (uint8_t)ISOTP_BS_TEST_PDU_LEN; i++) {
        out[i] = (uint8_t)(0x01U + i);
    }
}

/** Feed the FF (6 payload bytes) of the shared fixture into @p ctx. */
static uds_status_t bs_test_send_ff(isotp_ctx_t *ctx, const uint8_t *payload)
{
    uint8_t ff[8];
    uds_can_frame_t f;

    ff[0] = 0x10U;                              /* FF, FF_DL hi nibble = 0 */
    ff[1] = (uint8_t)ISOTP_BS_TEST_PDU_LEN;     /* FF_DL lo = 37           */
    memcpy(&ff[2], &payload[0], 6U);
    f = make_can_frame(0x7DFU, ff, 8U);
    return isotp_process_rx_frame(ctx, &f, rx_complete_cb, NULL);
}

/**
 * Feed CF number @p sn (1-based) of the shared fixture into @p ctx.
 * CF1..CF4 carry 7 bytes; CF5 carries the final 3.
 */
static uds_status_t bs_test_send_cf(isotp_ctx_t *ctx, const uint8_t *payload, uint8_t sn)
{
    uint8_t cf[8];
    uds_can_frame_t f;
    uint8_t  n      = (sn == 5U) ? 3U : 7U;
    uint32_t offset = 6U + ((uint32_t)(sn - 1U) * 7U);

    cf[0] = (uint8_t)(0x20U | (sn & 0x0FU));
    memcpy(&cf[1], &payload[offset], (size_t)n);
    f = make_can_frame(0x7DFU, cf, (uint8_t)(n + 1U));
    return isotp_process_rx_frame(ctx, &f, rx_complete_cb, NULL);
}

/** Byte-check that @p f is a well-formed FC CTS echoing @p bs / @p stmin. */
static void bs_test_assert_fc_cts(const uds_can_frame_t *f, uint8_t bs, uint8_t stmin,
                                  const char *what)
{
    zassert_equal(f->id, 0x7E8U, "%s: FC must go out on the TX CAN ID", what);
    zassert_equal((f->data[0] >> 4U), (uint8_t)ISOTP_FRAME_TYPE_FC,
                  "%s: PCI type nibble must be FC (3)", what);
    zassert_equal((f->data[0] & 0x0FU), (uint8_t)ISOTP_FC_STATUS_CONTINUE_TO_SEND,
                  "%s: FlowStatus must be CTS (0)", what);
    zassert_equal(f->data[1], bs, "%s: FC must echo the configured BlockSize", what);
    zassert_equal(f->data[2], stmin, "%s: FC must echo the configured STmin", what);
#if !ISOTP_TX_PADDING
    zassert_equal(f->dlc, 3U, "%s: unpadded FC is 3 bytes", what);
#endif
}

/**
 * TC-ISTP-RX-MF-005 [#121]: with a non-zero advertised BlockSize the receiver
 * must send a further FC CTS after every BS consecutive frames
 * (ISO 15765-2 §9.6.5).
 *
 * BS = 2, STmin = 10 ms, 37-byte PDU (FF + 5 CFs).  Expected FC frames:
 *   [0] immediately after the FF,
 *   [1] after CF2  — closes block 1,
 *   [2] after CF4  — closes block 2,
 *   none after CF5 — the PDU is complete, the sender has nothing left to send.
 *
 * Before the fix the CF branch never called isotp_send_fc(), so only the
 * FF's CTS was ever emitted and a BS-honouring tester stalled at CF2 until
 * its own N_Bs expired.
 */
ZTEST(test_isotp_rx_multi, test_rx_block_size_periodic_fc)
{
    uint8_t payload[ISOTP_BS_TEST_PDU_LEN];
    isotp_ctx_t ctx;
    isotp_state_t state;

    mock_can_reset();
    rx_cb_reset();
    memset(&ctx, 0, sizeof(ctx));
    bs_test_make_payload(payload);
    zassert_equal(init_isotp_bs(&ctx, 2U, 10U), UDS_STATUS_OK, "init failed");

    /* FF → initial CTS. */
    zassert_equal(bs_test_send_ff(&ctx, payload), UDS_STATUS_OK, "FF rejected");
    zassert_equal(g_mock_tx_count, 1U, "FF must trigger exactly one FC");
    bs_test_assert_fc_cts(&g_mock_tx_frames[0], 2U, 10U, "FC after FF");

    /* CF1 — mid-block, no FC. */
    zassert_equal(bs_test_send_cf(&ctx, payload, 1U), UDS_STATUS_OK, "CF1 rejected");
    zassert_equal(g_mock_tx_count, 1U, "No FC may be sent mid-block (after CF1)");

    /* CF2 — closes block 1, a further FC CTS is required. */
    zassert_equal(bs_test_send_cf(&ctx, payload, 2U), UDS_STATUS_OK, "CF2 rejected");
    zassert_equal(g_mock_tx_count, 2U,
                  "ISO 15765-2 9.6.5: a further FC is required after BS=2 CFs");
    bs_test_assert_fc_cts(&g_mock_tx_frames[1], 2U, 10U, "FC after CF2");
    zassert_false(g_rx_cb_called, "PDU is not complete yet");

    /* CF3 — mid-block, no FC (proves the counter reset, not a latch). */
    zassert_equal(bs_test_send_cf(&ctx, payload, 3U), UDS_STATUS_OK, "CF3 rejected");
    zassert_equal(g_mock_tx_count, 2U, "No FC may be sent mid-block (after CF3)");

    /* CF4 — closes block 2. */
    zassert_equal(bs_test_send_cf(&ctx, payload, 4U), UDS_STATUS_OK, "CF4 rejected");
    zassert_equal(g_mock_tx_count, 3U, "A third FC is required after the 2nd block");
    bs_test_assert_fc_cts(&g_mock_tx_frames[2], 2U, 10U, "FC after CF4");

    /* CF5 — completes the PDU; no trailing FC. */
    zassert_equal(bs_test_send_cf(&ctx, payload, 5U), UDS_STATUS_OK, "CF5 rejected");
    zassert_equal(g_mock_tx_count, 3U,
                  "No FC may follow the final CF — the PDU is complete");

    zassert_true(g_rx_cb_called, "Callback must fire once the PDU is complete");
    zassert_equal(g_rx_cb_len, (uint32_t)ISOTP_BS_TEST_PDU_LEN,
                  "Reassembled length must be 37");
    zassert_equal(memcmp(g_rx_cb_data, payload, (size_t)ISOTP_BS_TEST_PDU_LEN), 0,
                  "Reassembled payload must be byte-identical to what was sent");

    zassert_equal(isotp_get_state(&ctx, &state), UDS_STATUS_OK, "get_state failed");
    zassert_equal(state, ISOTP_STATE_IDLE, "RX must return to IDLE");
}

/**
 * TC-ISTP-RX-MF-006 [#121] non-regression: BlockSize 0 means "unlimited", so
 * the receiver must send exactly ONE FC (the FF's CTS) for the whole PDU and
 * behave byte-for-byte as it did before the #121 fix.  This is the default
 * configuration (ISOTP_DEFAULT_BLOCK_SIZE == 0) used by every bundled example.
 *
 * Same 37-byte / 5-CF transfer as TC-ISTP-RX-MF-005, so the two tests differ
 * only in the advertised BlockSize.
 */
ZTEST(test_isotp_rx_multi, test_rx_block_size_zero_single_fc)
{
    uint8_t payload[ISOTP_BS_TEST_PDU_LEN];
    isotp_ctx_t ctx;
    uint8_t sn;

    mock_can_reset();
    rx_cb_reset();
    memset(&ctx, 0, sizeof(ctx));
    bs_test_make_payload(payload);
    /* init_isotp() is the shared BS=0 / STmin=0 default helper. */
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    zassert_equal(bs_test_send_ff(&ctx, payload), UDS_STATUS_OK, "FF rejected");
    zassert_equal(g_mock_tx_count, 1U, "FF must trigger exactly one FC");
    bs_test_assert_fc_cts(&g_mock_tx_frames[0], 0U, 0U, "FC after FF (BS=0)");

    for (sn = 1U; sn <= 5U; sn++) {
        zassert_equal(bs_test_send_cf(&ctx, payload, sn), UDS_STATUS_OK,
                      "CF rejected");
        zassert_equal(g_mock_tx_count, 1U,
                      "BS=0 is unlimited: no further FC may ever be sent");
    }

    zassert_true(g_rx_cb_called, "Callback must fire once the PDU is complete");
    zassert_equal(g_rx_cb_len, (uint32_t)ISOTP_BS_TEST_PDU_LEN,
                  "Reassembled length must be 37");
    zassert_equal(memcmp(g_rx_cb_data, payload, (size_t)ISOTP_BS_TEST_PDU_LEN), 0,
                  "Reassembled payload must be byte-identical to what was sent");
}

/**
 * TC-ISTP-RX-MF-005 [#122]: FF whose FC CTS transmit is rejected by the CAN
 *                            controller must NOT be reported as success.
 *
 * ISO 15765-2 Table 5 gives the receiver an N_Ar window for the Flow Control
 * frame it transmits. If the data link layer rejects that frame (bus-off, full
 * TX mailbox, arbitration loss past the driver's own timeout), flow control was
 * never granted: the sender never saw an FC and will never send a CF.
 *
 * Returning UDS_STATUS_OK and entering ISOTP_STATE_RX_WAIT_CF makes the ECU
 * burn its full N_Cr (150 ms) waiting for consecutive frames that cannot come,
 * while the real fault -- a local transmit failure the platform layer DID
 * report -- is visible nowhere.
 */
ZTEST(test_isotp_rx_multi, test_ff_fc_cts_tx_failure_is_reported)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    /* Reject ONLY the Flow Control frame; SF/FF/CF transmits still succeed. */
    g_mock_fail_on_fc = true;

    /* FF: total_len = 10 bytes, first 6 bytes = 0x01..0x06 */
    uint8_t ff_payload[] = { 0x10U, 0x0AU, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U };
    uds_can_frame_t ff = make_can_frame(0x7DFU, ff_payload, 8U);
    uds_status_t rc = isotp_process_rx_frame(&ctx, &ff, rx_complete_cb, NULL);

    /* The FC must genuinely have been attempted -- otherwise this test is vacuous. */
    zassert_true(g_mock_tx_count >= 1U, "FC CTS must be attempted after an FF");
    zassert_equal((g_mock_tx_frames[0].data[0] >> 4U), (uint8_t)ISOTP_FRAME_TYPE_FC,
                  "Attempted frame must be of FC type");

    zassert_not_equal(rc, UDS_STATUS_OK,
                      "A rejected FC transmit must not be reported as success");
    zassert_equal(rc, UDS_STATUS_ERR_TP_TX_FAILED,
                  "FC transmit failure must surface as ERR_TP_TX_FAILED");

    isotp_state_t state;
    zassert_equal(isotp_get_state(&ctx, &state), UDS_STATUS_OK, "get_state failed");
    zassert_not_equal(state, ISOTP_STATE_RX_WAIT_CF,
                      "Must not await CFs that flow control never authorised");
    zassert_equal(state, ISOTP_STATE_ERROR,
                  "RX channel must go to ERROR after a failed FC transmit");
    zassert_false(g_rx_cb_called, "No callback for an aborted reassembly");
}

/**
 * TC-ISTP-RX-MF-006 [#122]: control case -- the FC failure injection used by
 * TC-ISTP-RX-MF-005 is specific to Flow Control frames.
 *
 * A Single Frame reception transmits nothing at all, so it must be completely
 * unaffected while g_mock_fail_on_fc is set. This pins the injection to the FC
 * path and rules out a blanket "all transmits fail" reading of MF-005.
 */
ZTEST(test_isotp_rx_multi, test_fc_tx_failure_injection_is_fc_specific)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    g_mock_fail_on_fc = true;

    uint8_t sf_payload[] = { 0x03U, 0xAAU, 0xBBU, 0xCCU, 0x00U, 0x00U, 0x00U, 0x00U };
    uds_can_frame_t sf = make_can_frame(0x7DFU, sf_payload, 8U);
    uds_status_t rc = isotp_process_rx_frame(&ctx, &sf, rx_complete_cb, NULL);

    zassert_equal(rc, UDS_STATUS_OK, "SF RX sends no FC and must still succeed");
    zassert_true(g_rx_cb_called, "SF callback must fire");
    zassert_equal(g_rx_cb_len, 3U, "SF payload length mismatch");
    zassert_equal(g_mock_tx_count, 0U, "SF RX must not transmit any frame");
}

/**
 * TC-ISTP-RX-MF-007 [#122]: N_Ar is armed across the FC transmit and stopped on
 *                            its confirmation -- and never leaks into N_Cr.
 *
 * N_Ar (ISO 15765-2 Table 5, 25 ms) is the receiver-side mirror of N_As: it
 * measures ONE frame's request-to-confirmation window, here the FC the receiver
 * transmits. The timer is only observable from inside can_transport_transmit(),
 * which is where the mock samples it; after the call returns it must be zero,
 * and the wait that follows belongs to N_Cr, not N_Ar.
 */
ZTEST(test_isotp_rx_multi, test_n_ar_armed_across_fc_transmit_only)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    g_mock_observed_ctx = &ctx;

    uint8_t ff_payload[] = { 0x10U, 0x0AU, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U };
    uds_can_frame_t ff = make_can_frame(0x7DFU, ff_payload, 8U);
    zassert_equal(isotp_process_rx_frame(&ctx, &ff, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "FF with a successful FC must succeed");

    zassert_true(g_mock_fc_observed, "FC transmit must have been observed");
    zassert_equal(g_mock_ar_timer_during_fc, (uint32_t)ISOTP_TIMEOUT_AR_MS,
                  "N_Ar must be armed for the duration of the FC transmit");
    zassert_equal(ctx.rx_ar_timer_ms, 0U,
                  "N_Ar must stop on the FC transmission confirmation");

    /* The window that follows is N_Cr, not N_Ar -- ticking must not fire N_Ar. */
    zassert_equal(isotp_tick_1ms(&ctx), UDS_STATUS_OK,
                  "Tick in RX_WAIT_CF must not report an N_Ar timeout");
    zassert_equal(ctx.rx_ar_timer_ms, 0U, "N_Ar must stay stopped in RX_WAIT_CF");
}

/**
 * TC-ISTP-RX-MF-007 [#121 x #122 reconciliation]: the periodic block-boundary
 * FC that BlockSize handling (#121) sends from the CF branch is a THIRD
 * isotp_send_fc() call site, alongside the two the FF handler already covers
 * (TC-ISTP-RX-MF-005/006 above). It must be held to the same standard: a
 * rejected transmit must not be silently absorbed.
 *
 * Uses the BS=2 fixture from the block-size regression tests: FF, CF1 (no FC
 * -- block not yet full), then CF2, which crosses the BS=2 boundary and must
 * trigger a fresh FC CTS. Only THAT FC is made to fail -- the FF's initial
 * CTS must still succeed, or this test would not isolate the third call site.
 */
ZTEST(test_isotp_rx_multi, test_periodic_block_boundary_fc_tx_failure_is_reported)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    uint8_t     payload[ISOTP_BS_TEST_PDU_LEN];
    memset(&ctx, 0, sizeof(ctx));
    bs_test_make_payload(payload);
    zassert_equal(init_isotp_bs(&ctx, 2U, 0U), UDS_STATUS_OK, "init failed");

    zassert_equal(bs_test_send_ff(&ctx, payload), UDS_STATUS_OK,
                  "FF with a successful FC must succeed");
    zassert_equal(g_mock_tx_count, 1U, "FF's FC must be the only frame sent so far");

    zassert_equal(bs_test_send_cf(&ctx, payload, 1U), UDS_STATUS_OK,
                  "CF1 must be accepted; block not yet full at BS=2");
    zassert_equal(g_mock_tx_count, 1U, "CF1 alone must not yet trigger a block FC");

    /* Reject only the FC that CF2 is about to trigger. */
    g_mock_fail_on_fc = true;

    uds_status_t rc = bs_test_send_cf(&ctx, payload, 2U);

    zassert_equal(g_mock_tx_count, 2U,
                  "The block-boundary FC must genuinely have been attempted");
    zassert_equal((g_mock_tx_frames[1].data[0] >> 4U), (uint8_t)ISOTP_FRAME_TYPE_FC,
                  "The second attempted frame must be of FC type");

    zassert_not_equal(rc, UDS_STATUS_OK,
                      "A rejected block-boundary FC must not be reported as success");
    zassert_equal(rc, UDS_STATUS_ERR_TP_TX_FAILED,
                  "Block-boundary FC transmit failure must surface as ERR_TP_TX_FAILED");

    isotp_state_t state;
    zassert_equal(isotp_get_state(&ctx, &state), UDS_STATUS_OK, "get_state failed");
    zassert_equal(state, ISOTP_STATE_ERROR,
                  "RX channel must go to ERROR after a failed block-boundary FC transmit");
    zassert_false(g_rx_cb_called, "No callback for an aborted reassembly");
}

#if ISOTP_ENABLE_CAN_FD
/**
 * TC-ISTP-RX-MF-004: CAN FD FF escape sequence with FF_DL > ISOTP_RX_BUF_LEN
 *                     → UDS_STATUS_ERR_TP_OVERFLOW + FC OVFLW transmitted.
 *
 * Sends an FD FF with 32-bit FF_DL = 5000 (> 4095 = ISOTP_RX_BUF_LEN).
 * Bytes 0-1 = 0x10 0x00 (escape); bytes 2-5 = big-endian 5000 = 0x00001388.
 */
ZTEST(test_isotp_rx_multi, test_ff_overflow)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp_fd(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t ff_payload[] = {
        0x10U, 0x00U,               /* FF type, escape sequence trigger */
        0x00U, 0x00U, 0x13U, 0x88U, /* FF_DL = 5000 (big-endian) */
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U  /* first data bytes */
    };
    uds_can_frame_t ff = make_fd_frame(0x7DFU, ff_payload, 12U);
    uds_status_t rc = isotp_process_rx_frame(&ctx, &ff, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_TP_OVERFLOW,
                  "FD FF with FF_DL > ISOTP_RX_BUF_LEN must send OVFLW and fail");
    /* FC OVFLW must have been transmitted */
    zassert_true(g_mock_tx_count >= 1U, "FC OVFLW frame must be sent");
    zassert_equal((g_mock_tx_frames[0].data[0] >> 4U), (uint8_t)ISOTP_FRAME_TYPE_FC,
                  "Transmitted frame must be FC type");
    zassert_equal((g_mock_tx_frames[0].data[0] & 0x0FU), (uint8_t)ISOTP_FC_STATUS_OVERFLOW,
                  "FC status must be OVERFLOW");
}

/**
 * TC-ISTP-RX-MF-008 [#122]: FF_DL overflow whose FC OVFLW transmit is itself
 *                            rejected must report the transmit failure.
 *
 * Reachable only under CAN FD: on Classic CAN the 12-bit FF_DL cannot exceed
 * ISOTP_RX_BUF_LEN (4095), so the OVERFLOW branch is dead there.
 *
 * A rejected FC transmit means the local CAN link is down (bus-off / mailbox
 * full), which invalidates the channel -- not merely this one PDU. Returning
 * ERR_TP_OVERFLOW would report the peer's protocol condition while hiding our
 * own hardware fault, which is the exact inversion #122 is about.
 */
ZTEST(test_isotp_rx_multi, test_ff_overflow_fc_tx_failure_is_reported)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp_fd(&ctx), UDS_STATUS_OK, "init failed");

    g_mock_fail_on_fc = true;

    uint8_t ff_payload[] = {
        0x10U, 0x00U,               /* FF type, escape sequence trigger */
        0x00U, 0x00U, 0x13U, 0x88U, /* FF_DL = 5000 (big-endian) */
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U
    };
    uds_can_frame_t ff = make_fd_frame(0x7DFU, ff_payload, 12U);
    uds_status_t rc = isotp_process_rx_frame(&ctx, &ff, rx_complete_cb, NULL);

    zassert_true(g_mock_tx_count >= 1U, "FC OVFLW must be attempted");
    zassert_equal((g_mock_tx_frames[0].data[0] & 0x0FU), (uint8_t)ISOTP_FC_STATUS_OVERFLOW,
                  "Attempted FC must carry OVFLW");
    zassert_equal(rc, UDS_STATUS_ERR_TP_TX_FAILED,
                  "A rejected FC OVFLW transmit must surface as ERR_TP_TX_FAILED");

    isotp_state_t state;
    zassert_equal(isotp_get_state(&ctx, &state), UDS_STATUS_OK, "get_state failed");
    zassert_equal(state, ISOTP_STATE_ERROR,
                  "RX channel must go to ERROR after a failed FC transmit");
}
#endif /* ISOTP_ENABLE_CAN_FD — test_ff_overflow */

/* =========================================================================
 * Test suite: CAN FD SF and FF (ISO 15765-2 §9.8)
 * ========================================================================= */

#if ISOTP_ENABLE_CAN_FD

ZTEST_SUITE(test_isotp_canfd, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-ISTP-FD-001: CAN FD SF receive — 10-byte payload.
 * FD SF encoding: byte 0 = 0x00, byte 1 = SF_DL (10), data at [2..11].
 */
ZTEST(test_isotp_canfd, test_fd_sf_rx_10_bytes)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp_fd(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t payload[12] = {
        0x00U, 0x0AU,                                       /* FD SF PCI: SF_DL = 10 */
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U,
        0x06U, 0x07U, 0x08U, 0x09U, 0x0AU                  /* 10 data bytes */
    };
    uds_can_frame_t f = make_fd_frame(0x7DFU, payload, 12U);

    uds_status_t rc = isotp_process_rx_frame(&ctx, &f, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "FD SF RX must succeed");
    zassert_true(g_rx_cb_called, "Callback must fire");
    zassert_equal(g_rx_cb_len, 10U, "Length must be 10");
    zassert_equal(g_rx_cb_data[0], 0x01U, "data[0] mismatch");
    zassert_equal(g_rx_cb_data[9], 0x0AU, "data[9] mismatch");
}

/**
 * TC-ISTP-FD-002: CAN FD SF receive — maximum 62-byte payload.
 */
ZTEST(test_isotp_canfd, test_fd_sf_rx_62_bytes)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp_fd(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t payload[64];
    memset(payload, 0, sizeof(payload));
    payload[0] = 0x00U;  /* FD SF escape */
    payload[1] = 0x3EU;  /* SF_DL = 62 */
    memset(&payload[2], 0xAAU, 62U);

    uds_can_frame_t f = make_fd_frame(0x7DFU, payload, 64U);
    uds_status_t rc = isotp_process_rx_frame(&ctx, &f, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "FD SF 62-byte RX must succeed");
    zassert_true(g_rx_cb_called, "Callback must fire");
    zassert_equal(g_rx_cb_len, 62U, "Length must be 62");
    zassert_equal(g_rx_cb_data[0],  0xAAU, "data[0] mismatch");
    zassert_equal(g_rx_cb_data[61], 0xAAU, "data[61] mismatch");
}

/**
 * TC-ISTP-FD-003: CAN FD SF receive — SF_DL = 0 on FD frame → FRAME_INVALID.
 * byte 0 = 0x00, byte 1 = 0x00 → SF_DL zero is invalid.
 */
ZTEST(test_isotp_canfd, test_fd_sf_rx_zero_dl)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp_fd(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t payload[12] = { 0x00U, 0x00U };
    uds_can_frame_t f = make_fd_frame(0x7DFU, payload, 12U);
    uds_status_t rc = isotp_process_rx_frame(&ctx, &f, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_TP_FRAME_INVALID,
                  "FD SF with SF_DL=0 must be rejected");
    zassert_false(g_rx_cb_called, "Callback must not fire");
}

/**
 * TC-ISTP-FD-004: CAN FD SF transmit — 10-byte payload.
 * Verifies FD SF encoding: byte 0 = 0x00, byte 1 = 10.
 */
ZTEST(test_isotp_canfd, test_fd_sf_tx_10_bytes)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp_fd(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t data[10];
    memset(data, 0xBBU, sizeof(data));

    uds_status_t rc = isotp_transmit(&ctx, data, 10U);
    zassert_equal(rc, UDS_STATUS_OK, "FD SF TX must succeed");
    zassert_equal(g_mock_tx_count, 1U, "Exactly one CAN frame must be sent");
    zassert_true(g_mock_tx_frames[0].is_fd, "Transmitted frame must be CAN FD");
    zassert_equal(g_mock_tx_frames[0].data[0], 0x00U, "FD SF byte 0 must be 0x00");
    zassert_equal(g_mock_tx_frames[0].data[1], 10U,   "FD SF byte 1 must be SF_DL=10");
    zassert_equal(g_mock_tx_frames[0].data[2], 0xBBU, "First data byte mismatch");

    isotp_state_t state;
    isotp_get_state(&ctx, &state);
    zassert_equal(state, ISOTP_STATE_IDLE, "State must return to IDLE after FD SF TX");
}

/**
 * TC-ISTP-FD-005: CAN FD FF escape sequence receive — FF_DL = 5000 bytes
 *                  within ISOTP_RX_BUF_LEN → not possible without expanding
 *                  the buffer; test that FF_DL = 100 (fits) works.
 *
 * Sends an FD FF with escape sequence: bytes 0-1 = 0x10 0x00,
 * bytes 2-5 = 0x00000064 (100 decimal), then 10 first data bytes.
 */
ZTEST(test_isotp_canfd, test_fd_ff_escape_rx_fits)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp_fd(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t ff_payload[16] = {
        0x10U, 0x00U,               /* FD FF escape */
        0x00U, 0x00U, 0x00U, 0x64U, /* FF_DL = 100 */
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U, 0x0AU
    };
    uds_can_frame_t ff = make_fd_frame(0x7DFU, ff_payload, 16U);
    uds_status_t rc = isotp_process_rx_frame(&ctx, &ff, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "FD FF escape RX must be accepted");
    zassert_false(g_rx_cb_called, "Callback must NOT fire after FF alone");

    isotp_state_t state;
    isotp_get_state(&ctx, &state);
    zassert_equal(state, ISOTP_STATE_RX_WAIT_CF, "Must be in RX_WAIT_CF");
    /* FC CTS must have been sent */
    zassert_true(g_mock_tx_count >= 1U, "FC CTS must be transmitted");
    zassert_equal((g_mock_tx_frames[0].data[0] >> 4U), (uint8_t)ISOTP_FRAME_TYPE_FC,
                  "Transmitted frame must be FC type");
    zassert_equal((g_mock_tx_frames[0].data[0] & 0x0FU),
                  (uint8_t)ISOTP_FC_STATUS_CONTINUE_TO_SEND,
                  "FC status must be CTS");
}

/**
 * TC-ISTP-FD-006: CAN FD FF escape — FF_DL > ISOTP_RX_BUF_LEN → FC OVFLW.
 * Duplicate of test_ff_overflow but via the FD-specific suite for clarity.
 */
ZTEST(test_isotp_canfd, test_fd_ff_escape_rx_overflow)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp_fd(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t ff_payload[] = {
        0x10U, 0x00U,
        0x00U, 0x00U, 0x13U, 0x88U,  /* FF_DL = 5000 > 4095 */
        0x01U, 0x02U
    };
    uds_can_frame_t ff = make_fd_frame(0x7DFU, ff_payload, 8U);
    uds_status_t rc = isotp_process_rx_frame(&ctx, &ff, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_TP_OVERFLOW, "Must overflow when FF_DL > buf");
}

/**
 * TC-ISTP-FD-007: CAN FD FF escape on Classic CAN frame → FRAME_INVALID.
 * FF_DL == 0 on a non-FD frame must be rejected.
 */
ZTEST(test_isotp_canfd, test_fd_ff_escape_classic_can_rejected)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    /* Classic CAN frame with byte 0 = 0x10, byte 1 = 0x00 → escape not valid */
    uint8_t payload[] = { 0x10U, 0x00U, 0x00U, 0x00U, 0x00U, 0x64U, 0x01U, 0x02U };
    uds_can_frame_t f = make_can_frame(0x7DFU, payload, 8U);
    uds_status_t rc = isotp_process_rx_frame(&ctx, &f, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_TP_FRAME_INVALID,
                  "FF_DL=0 on Classic CAN must be FRAME_INVALID");
}

/**
 * TC-ISTP-FD-008: CAN FD FF escape TX — length > UDS_MAX_PAYLOAD_LEN (4095).
 * Verifies escape encoding: byte 0 = 0x10, byte 1 = 0x00, bytes 2-5 = length.
 */
ZTEST(test_isotp_canfd, test_fd_ff_escape_tx)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp_fd(&ctx), UDS_STATUS_OK, "init failed");

    static uint8_t s_large_data[5000];
    memset(s_large_data, 0xCCU, sizeof(s_large_data));

    uds_status_t rc = isotp_transmit(&ctx, s_large_data, 5000U);
    zassert_equal(rc, UDS_STATUS_OK, "FD FF escape TX must succeed");
    zassert_true(g_mock_tx_count >= 1U, "FF must be transmitted");

    /* Verify escape encoding */
    zassert_true(g_mock_tx_frames[0].is_fd, "FF must be a CAN FD frame");
    zassert_equal(g_mock_tx_frames[0].data[0], 0x10U, "FF byte 0 must be 0x10");
    zassert_equal(g_mock_tx_frames[0].data[1], 0x00U, "FF byte 1 must be 0x00");
    /* 5000 = 0x00001388 */
    zassert_equal(g_mock_tx_frames[0].data[2], 0x00U, "FF_DL byte 2 mismatch");
    zassert_equal(g_mock_tx_frames[0].data[3], 0x00U, "FF_DL byte 3 mismatch");
    zassert_equal(g_mock_tx_frames[0].data[4], 0x13U, "FF_DL byte 4 mismatch");
    zassert_equal(g_mock_tx_frames[0].data[5], 0x88U, "FF_DL byte 5 mismatch");

    isotp_state_t state;
    isotp_get_state(&ctx, &state);
    zassert_equal(state, ISOTP_STATE_TX_WAIT_FC,
                  "State must be TX_WAIT_FC after FD FF escape TX");
}
#endif /* ISOTP_ENABLE_CAN_FD — test_isotp_canfd suite */

/* =========================================================================
 * Test suite: isotp_transmit
 * ========================================================================= */

ZTEST_SUITE(test_isotp_transmit, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-ISTP-TX-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_isotp_transmit, test_null_ctx)
{
    uint8_t data[] = { 0x50U, 0x03U };
    uds_status_t rc = isotp_transmit(NULL, data, 2U);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must fail");
}

/**
 * TC-ISTP-TX-002: NULL data → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_isotp_transmit, test_null_data)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");
    uds_status_t rc = isotp_transmit(&ctx, NULL, 4U);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL data must fail");
}

/**
 * TC-ISTP-TX-003: Zero length → UDS_STATUS_ERR_INVALID_PARAM.
 */
ZTEST(test_isotp_transmit, test_zero_length)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");
    uint8_t data[] = { 0x50U };
    uds_status_t rc = isotp_transmit(&ctx, data, 0U);
    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM, "Zero length must fail");
}

/**
 * TC-ISTP-TX-004: Classic CAN, length > UDS_MAX_PAYLOAD_LEN →
 *                 UDS_STATUS_ERR_BUFFER_OVERFLOW.
 */
ZTEST(test_isotp_transmit, test_overflow_length)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");
    uint8_t data[8] = { 0 };
    uds_status_t rc = isotp_transmit(&ctx, data, (uint32_t)(UDS_MAX_PAYLOAD_LEN + 1U));
    zassert_equal(rc, UDS_STATUS_ERR_BUFFER_OVERFLOW, "Classic CAN overflow must fail");
}

/**
 * TC-ISTP-TX-005: Single-frame transmit (3 bytes — DiagnosticSessionControl positive).
 * Response [0x50, 0x03, 0x00, 0x19, 0x01, 0xF4] = 6 bytes → still single-frame.
 */
ZTEST(test_isotp_transmit, test_single_frame_transmit)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t data[] = { 0x50U, 0x03U, 0x00U, 0x19U, 0x01U, 0xF4U };
    uds_status_t rc = isotp_transmit(&ctx, data, 6U);
    zassert_equal(rc, UDS_STATUS_OK, "Single-frame TX must succeed");
    /* A single CAN frame must have been transmitted */
    zassert_true(g_mock_tx_count >= 1U, "At least one CAN frame must be sent");
    /* SF PCI: upper nibble = 0x0 (SF), lower nibble = data length */
    zassert_equal((g_mock_tx_frames[0].data[0] >> 4U), (uint8_t)ISOTP_FRAME_TYPE_SF,
                  "TX frame must have SF PCI");
    zassert_equal((g_mock_tx_frames[0].data[0] & 0x0FU), 6U,
                  "SF length nibble must be 6");
}

/**
 * TC-ISTP-TX-006: Multi-frame transmit start — 17-byte VIN response.
 * The first transmission must produce a First Frame (FF) PCI.
 */
ZTEST(test_isotp_transmit, test_multi_frame_ff)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    /* VIN response: [0x62, 0xF1, 0x90, <17 bytes>] = 20 bytes total */
    uint8_t data[20];
    data[0] = 0x62U; data[1] = 0xF1U; data[2] = 0x90U;
    memset(&data[3], 0x41U, 17U);  /* 'A' × 17 */

    uds_status_t rc = isotp_transmit(&ctx, data, 20U);
    zassert_equal(rc, UDS_STATUS_OK, "Multi-frame TX initiation must succeed");
    zassert_true(g_mock_tx_count >= 1U, "FF must be sent");

    /* FF PCI: upper nibble = 0x1, lower nibble = hi bits of length */
    zassert_equal((g_mock_tx_frames[0].data[0] >> 4U), (uint8_t)ISOTP_FRAME_TYPE_FF,
                  "First CAN frame must have FF PCI");

    /* State should move to TX_WAIT_FC */
    isotp_state_t state;
    isotp_get_state(&ctx, &state);
    /* If FC handling is stubbed, state may still reflect TX in progress */
    zassert_not_equal(state, ISOTP_STATE_IDLE,
                      "State must not be IDLE during multi-frame TX");
}

/**
 * TC-ISTP-TX-007: TX while already busy → UDS_STATUS_ERR_BUSY.
 */
ZTEST(test_isotp_transmit, test_tx_busy)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t data[20];
    memset(data, 0xBBU, sizeof(data));

    /* Start a multi-frame TX — leaves state as TX_WAIT_FC */
    zassert_equal(isotp_transmit(&ctx, data, 20U), UDS_STATUS_OK, "First TX must succeed");

    /* Second TX attempt while busy must fail */
    uds_status_t rc = isotp_transmit(&ctx, data, 20U);
    zassert_equal(rc, UDS_STATUS_ERR_BUSY,
                  "Transmit while TX in progress must return ERR_BUSY");
}

/* =========================================================================
 * Test suite: isotp_tick_1ms
 * ========================================================================= */

ZTEST_SUITE(test_isotp_tick, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-ISTP-TICK-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_isotp_tick, test_null_ctx)
{
    uds_status_t rc = isotp_tick_1ms(NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must fail");
}

/**
 * TC-ISTP-TICK-002: Cr timeout — tick ISOTP_TIMEOUT_CR_MS+1 times while
 *                   in RX_WAIT_CF state → UDS_STATUS_ERR_TP_TIMEOUT_CR.
 */
ZTEST(test_isotp_tick, test_cr_timeout)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    /* Inject FF to enter RX_WAIT_CF */
    uint8_t ff_payload[] = { 0x10U, 0x0AU, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U };
    uds_can_frame_t ff = make_can_frame(0x7DFU, ff_payload, 8U);
    zassert_equal(isotp_process_rx_frame(&ctx, &ff, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "FF inject failed");

    isotp_state_t state;
    isotp_get_state(&ctx, &state);
    zassert_equal(state, ISOTP_STATE_RX_WAIT_CF, "Must be in RX_WAIT_CF");

    /* Tick until Cr timeout fires */
    uds_status_t tick_rc = UDS_STATUS_OK;
    uint32_t ticks;
    for (ticks = 0U; ticks <= (uint32_t)(ISOTP_TIMEOUT_CR_MS + 2U); ticks++) {
        tick_rc = isotp_tick_1ms(&ctx);
        if (tick_rc != UDS_STATUS_OK) {
            break;
        }
    }
    zassert_equal(tick_rc, UDS_STATUS_ERR_TP_TIMEOUT_CR,
                  "Cr timeout must return ERR_TP_TIMEOUT_CR");

    isotp_get_state(&ctx, &state);
    zassert_equal(state, ISOTP_STATE_ERROR,
                  "State must be ERROR after Cr timeout");
}

/**
 * TC-ISTP-TICK-003: Tick in IDLE state → UDS_STATUS_OK, no timeout.
 */
ZTEST(test_isotp_tick, test_tick_idle)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    for (int i = 0; i < 10; i++) {
        uds_status_t rc = isotp_tick_1ms(&ctx);
        zassert_equal(rc, UDS_STATUS_OK, "Tick in IDLE must return OK");
    }
}

/* =========================================================================
 * Test suite: isotp_reset
 * ========================================================================= */

ZTEST_SUITE(test_isotp_reset, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-ISTP-RST-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_isotp_reset, test_null_ctx)
{
    uds_status_t rc = isotp_reset(NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must fail");
}

/**
 * TC-ISTP-RST-002: Reset after error state → state returns to IDLE.
 */
ZTEST(test_isotp_reset, test_reset_from_error)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    /* Force Cr timeout to reach ERROR state */
    uint8_t ff_payload[] = { 0x10U, 0x0AU, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U };
    uds_can_frame_t ff = make_can_frame(0x7DFU, ff_payload, 8U);
    isotp_process_rx_frame(&ctx, &ff, rx_complete_cb, NULL);

    for (uint32_t i = 0U; i <= (uint32_t)(ISOTP_TIMEOUT_CR_MS + 2U); i++) {
        isotp_tick_1ms(&ctx);
    }

    isotp_state_t state;
    isotp_get_state(&ctx, &state);
    zassert_equal(state, ISOTP_STATE_ERROR, "Must be in ERROR before reset");

    uds_status_t rc = isotp_reset(&ctx);
    zassert_equal(rc, UDS_STATUS_OK, "Reset must return OK");

    isotp_get_state(&ctx, &state);
    zassert_equal(state, ISOTP_STATE_IDLE, "State must be IDLE after reset");
}

/* =========================================================================
 * Test suite: isotp_get_state
 * ========================================================================= */

ZTEST_SUITE(test_isotp_get_state, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-ISTP-GSTATE-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_isotp_get_state, test_null_ctx)
{
    isotp_state_t state;
    uds_status_t rc = isotp_get_state(NULL, &state);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must fail");
}

/**
 * TC-ISTP-GSTATE-002: NULL out_state → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_isotp_get_state, test_null_state_ptr)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");
    uds_status_t rc = isotp_get_state(&ctx, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL out_state must fail");
}

/* =========================================================================
 * Test suite: TX frame padding (ISOTP_TX_PADDING)
 * Tests only compiled and wired when ISOTP_TX_PADDING=1.
 * REQ-TP-PAD-001: unused frame bytes filled with ISOTP_TX_PADDING_BYTE.
 * ========================================================================= */

#if ISOTP_TX_PADDING

ZTEST_SUITE(test_isotp_padding, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-ISTP-PAD-001: Classic CAN SF — DLC must be 8, tail bytes must be 0xCC.
 * Payload 3 bytes: PCI=1 byte + data=3 bytes = 4 used; bytes [4..7] padded.
 */
ZTEST(test_isotp_padding, test_classic_sf_padded)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t payload[3] = { 0x62U, 0xF1U, 0x90U };
    uds_status_t rc = isotp_transmit(&ctx, payload, (uint32_t)3U);
    zassert_equal(rc, UDS_STATUS_OK, "SF TX must succeed");
    zassert_true(g_mock_tx_count >= 1U, "one frame must be sent");

    const uds_can_frame_t *f = &g_mock_tx_frames[0];
    zassert_equal(f->dlc, (uint8_t)8U, "padded Classic CAN SF DLC must be 8");
    zassert_equal(f->data[0] & 0x0FU, (uint8_t)3U, "SF_DL must still be 3");
    /* Bytes [4..7] must be the padding byte. */
    zassert_equal(f->data[4], (uint8_t)ISOTP_TX_PADDING_BYTE, "pad byte [4]");
    zassert_equal(f->data[5], (uint8_t)ISOTP_TX_PADDING_BYTE, "pad byte [5]");
    zassert_equal(f->data[6], (uint8_t)ISOTP_TX_PADDING_BYTE, "pad byte [6]");
    zassert_equal(f->data[7], (uint8_t)ISOTP_TX_PADDING_BYTE, "pad byte [7]");
}

/**
 * TC-ISTP-PAD-002: Classic CAN SF — maximum 7-byte payload, no tail to pad.
 * All 8 bytes occupied (PCI=1 + data=7). DLC=8, no padding byte visible.
 */
ZTEST(test_isotp_padding, test_classic_sf_max_payload_no_tail)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t payload[7] = { 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U };
    uds_status_t rc = isotp_transmit(&ctx, payload, (uint32_t)7U);
    zassert_equal(rc, UDS_STATUS_OK, "7-byte SF TX must succeed");

    const uds_can_frame_t *f = &g_mock_tx_frames[0];
    zassert_equal(f->dlc, (uint8_t)8U, "DLC must be 8");
    zassert_equal(f->data[0] & 0x0FU, (uint8_t)7U, "SF_DL must be 7");
    /* Payload bytes must be intact. */
    zassert_equal(f->data[1], 0x01U, "data[1]");
    zassert_equal(f->data[7], 0x07U, "data[7]");
}

/**
 * TC-ISTP-PAD-003: FC frame — DLC must be 8, bytes [3..7] must be 0xCC.
 * FC is emitted by the receiver when it gets a FF. Send an FF to trigger it.
 */
ZTEST(test_isotp_padding, test_fc_padded)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    /* Build a valid Classic CAN FF (20-byte payload). */
    uds_can_frame_t ff;
    memset(&ff, 0, sizeof(ff));
    ff.id      = ctx.rx_can_id;
    ff.dlc     = (uint8_t)8U;
    ff.data[0] = (uint8_t)0x10U | (uint8_t)((20U >> 8U) & 0x0FU); /* FF, FF_DL=20 */
    ff.data[1] = (uint8_t)(20U & 0xFFU);
    ff.data[2] = 0xAAU; ff.data[3] = 0xBBU; ff.data[4] = 0xCCU;
    ff.data[5] = 0xDDU; ff.data[6] = 0xEEU; ff.data[7] = 0xFFU;

    uds_status_t rc = isotp_process_rx_frame(&ctx, &ff, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "FF processing must succeed");
    zassert_true(g_mock_tx_count >= 1U, "FC must be sent");

    /* The FC frame is the last transmitted frame. */
    const uds_can_frame_t *fc = &g_mock_tx_frames[g_mock_tx_count - 1U];
    zassert_equal(fc->dlc, (uint8_t)8U, "padded FC DLC must be 8");
    zassert_equal((fc->data[0] >> 4U), (uint8_t)ISOTP_FRAME_TYPE_FC, "FC type nibble");
    zassert_equal(fc->data[3], (uint8_t)ISOTP_TX_PADDING_BYTE, "pad byte [3]");
    zassert_equal(fc->data[4], (uint8_t)ISOTP_TX_PADDING_BYTE, "pad byte [4]");
    zassert_equal(fc->data[7], (uint8_t)ISOTP_TX_PADDING_BYTE, "pad byte [7]");
}

/**
 * TC-ISTP-PAD-004: Classic CAN CF — DLC must be 8, tail must be 0xCC.
 * Drive a full multi-frame TX: send FF, respond with FC CTS, tick until CF.
 */
ZTEST(test_isotp_padding, test_classic_cf_padded)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    /* Initiate a 10-byte TX — requires one FF + one CF. */
    uint8_t payload[10];
    uint8_t i;
    for (i = 0U; i < (uint8_t)10U; i++) { payload[i] = i; }

    zassert_equal(isotp_transmit(&ctx, payload, (uint32_t)10U),
                  UDS_STATUS_OK, "multi-frame TX initiation must succeed");

    /* Feed FC CTS back to release the CF pump. */
    uds_can_frame_t fc;
    memset(&fc, 0, sizeof(fc));
    fc.id      = ctx.rx_can_id;
    fc.dlc     = (uint8_t)3U;
    fc.data[0] = (uint8_t)((uint8_t)ISOTP_FRAME_TYPE_FC << 4U)
                 | (uint8_t)ISOTP_FC_STATUS_CONTINUE_TO_SEND;
    fc.data[1] = (uint8_t)0U;  /* BS = 0 */
    fc.data[2] = (uint8_t)0U;  /* STmin = 0 */

    uds_status_t rc = isotp_process_rx_frame(&ctx, &fc, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "FC CTS processing must succeed");

    /* Tick once to trigger isotp_tx_pump() → CF sent. */
    mock_can_reset();
    rc = isotp_tick_1ms(&ctx);
    zassert_equal(rc, UDS_STATUS_OK, "tick must succeed");
    zassert_true(g_mock_tx_count >= 1U, "CF must be sent on tick");

    const uds_can_frame_t *cf = &g_mock_tx_frames[0];
    zassert_equal(cf->dlc, (uint8_t)8U, "padded CF DLC must be 8");
    zassert_equal((cf->data[0] >> 4U), (uint8_t)ISOTP_FRAME_TYPE_CF, "CF type nibble");
    /* CF carries bytes [6..9] of the payload (4 bytes) + PCI = 5 used; [5..7] padded. */
    zassert_equal(cf->data[5], (uint8_t)ISOTP_TX_PADDING_BYTE, "CF pad byte [5]");
    zassert_equal(cf->data[6], (uint8_t)ISOTP_TX_PADDING_BYTE, "CF pad byte [6]");
    zassert_equal(cf->data[7], (uint8_t)ISOTP_TX_PADDING_BYTE, "CF pad byte [7]");
}

#if ISOTP_ENABLE_CAN_FD
/**
 * TC-ISTP-PAD-005: CAN FD SF — 10-byte payload → DLC must be 12, tail 0xCC.
 * FD SF uses 2 PCI bytes + 10 data = 12 used bytes. Next FD DLC ≥ 12 is 12.
 * No padding needed, but DLC must be rounded to 12.
 */
ZTEST(test_isotp_padding, test_fd_sf_padded_no_tail)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    isotp_cfg_t cfg = {
        .rx_can_id = 0x7E8U,
        .tx_can_id = 0x7DFU,
        .block_size = 0U,
        .stmin_ms   = 0U,
        .use_fd     = true,
        .can        = &g_mock_can,
    };
    zassert_equal(isotp_init(&ctx, &cfg), UDS_STATUS_OK, "FD init failed");

    uint8_t payload[10];
    uint8_t i;
    for (i = 0U; i < (uint8_t)10U; i++) { payload[i] = i; }

    uds_status_t rc = isotp_transmit(&ctx, payload, (uint32_t)10U);
    zassert_equal(rc, UDS_STATUS_OK, "FD SF TX must succeed");
    zassert_true(g_mock_tx_count >= 1U, "one frame must be sent");

    const uds_can_frame_t *f = &g_mock_tx_frames[0];
    zassert_true(f->is_fd, "frame must be marked FD");
    /* 2 PCI + 10 data = 12 used; next FD DLC ≥ 12 is 12. */
    zassert_equal(f->dlc, (uint8_t)12U, "FD SF DLC must be 12 after padding");
    zassert_equal(f->data[0], (uint8_t)0x00U, "FD SF escape byte 0");
    zassert_equal(f->data[1], (uint8_t)10U,   "FD SF_DL must be 10");
}

/**
 * TC-ISTP-PAD-006: CAN FD SF — 9-byte payload → DLC rounded to 12, 1 pad byte.
 * 2 PCI + 9 data = 11 used; next FD DLC ≥ 11 is 12. data[11] = 0xCC.
 */
ZTEST(test_isotp_padding, test_fd_sf_padded_one_tail_byte)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    isotp_cfg_t cfg = {
        .rx_can_id  = 0x7E8U,
        .tx_can_id  = 0x7DFU,
        .block_size = 0U,
        .stmin_ms   = 0U,
        .use_fd     = true,
        .can        = &g_mock_can,
    };
    zassert_equal(isotp_init(&ctx, &cfg), UDS_STATUS_OK, "FD init failed");

    uint8_t payload[9];
    uint8_t i;
    for (i = 0U; i < (uint8_t)9U; i++) { payload[i] = (uint8_t)(i + 1U); }

    uds_status_t rc = isotp_transmit(&ctx, payload, (uint32_t)9U);
    zassert_equal(rc, UDS_STATUS_OK, "FD SF TX must succeed");

    const uds_can_frame_t *f = &g_mock_tx_frames[0];
    zassert_equal(f->dlc, (uint8_t)12U, "FD SF DLC must be 12 (9+2=11, round up)");
    zassert_equal(f->data[11], (uint8_t)ISOTP_TX_PADDING_BYTE, "pad byte at [11]");
}
#endif /* ISOTP_ENABLE_CAN_FD */

#endif /* ISOTP_TX_PADDING */

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_isotp_init__test_null_ctx(void);
extern void test_isotp_init__test_null_cfg(void);
extern void test_isotp_init__test_null_can(void);
extern void test_isotp_init__test_happy_path(void);
extern void test_isotp_init__test_double_init(void);
extern void test_isotp_rx_single__test_null_ctx(void);
extern void test_isotp_rx_single__test_null_frame(void);
extern void test_isotp_rx_single__test_single_frame_3_bytes(void);
extern void test_isotp_rx_single__test_sf_zero_length(void);
extern void test_isotp_rx_single__test_sf_seven_bytes(void);
extern void test_isotp_rx_multi__test_first_frame_triggers_fc(void);
extern void test_isotp_rx_multi__test_ff_cf_complete(void);
extern void test_isotp_rx_multi__test_cf_without_ff(void);
extern void test_isotp_rx_multi__test_rx_block_size_periodic_fc(void);
extern void test_isotp_rx_multi__test_rx_block_size_zero_single_fc(void);
extern void test_isotp_rx_multi__test_ff_fc_cts_tx_failure_is_reported(void);
extern void test_isotp_rx_multi__test_fc_tx_failure_injection_is_fc_specific(void);
extern void test_isotp_rx_multi__test_n_ar_armed_across_fc_transmit_only(void);
extern void test_isotp_rx_multi__test_periodic_block_boundary_fc_tx_failure_is_reported(void);
#if ISOTP_ENABLE_CAN_FD
extern void test_isotp_rx_multi__test_ff_overflow(void);
extern void test_isotp_rx_multi__test_ff_overflow_fc_tx_failure_is_reported(void);
extern void test_isotp_canfd__test_fd_sf_rx_10_bytes(void);
extern void test_isotp_canfd__test_fd_sf_rx_62_bytes(void);
extern void test_isotp_canfd__test_fd_sf_rx_zero_dl(void);
extern void test_isotp_canfd__test_fd_sf_tx_10_bytes(void);
extern void test_isotp_canfd__test_fd_ff_escape_rx_fits(void);
extern void test_isotp_canfd__test_fd_ff_escape_rx_overflow(void);
extern void test_isotp_canfd__test_fd_ff_escape_classic_can_rejected(void);
extern void test_isotp_canfd__test_fd_ff_escape_tx(void);
#endif /* ISOTP_ENABLE_CAN_FD */
extern void test_isotp_transmit__test_null_ctx(void);
extern void test_isotp_transmit__test_null_data(void);
extern void test_isotp_transmit__test_zero_length(void);
extern void test_isotp_transmit__test_overflow_length(void);
extern void test_isotp_transmit__test_single_frame_transmit(void);
extern void test_isotp_transmit__test_multi_frame_ff(void);
extern void test_isotp_transmit__test_tx_busy(void);
extern void test_isotp_tick__test_null_ctx(void);
extern void test_isotp_tick__test_cr_timeout(void);
extern void test_isotp_tick__test_tick_idle(void);
extern void test_isotp_reset__test_null_ctx(void);
extern void test_isotp_reset__test_reset_from_error(void);
extern void test_isotp_get_state__test_null_ctx(void);
extern void test_isotp_get_state__test_null_state_ptr(void);
#if ISOTP_TX_PADDING
extern void test_isotp_padding__test_classic_sf_padded(void);
extern void test_isotp_padding__test_classic_sf_max_payload_no_tail(void);
extern void test_isotp_padding__test_fc_padded(void);
extern void test_isotp_padding__test_classic_cf_padded(void);
#if ISOTP_ENABLE_CAN_FD
extern void test_isotp_padding__test_fd_sf_padded_no_tail(void);
extern void test_isotp_padding__test_fd_sf_padded_one_tail_byte(void);
#endif /* ISOTP_ENABLE_CAN_FD */
#endif /* ISOTP_TX_PADDING */

void run_all_tests(void)
{
    RUN_TEST(test_isotp_init__test_null_ctx);
    RUN_TEST(test_isotp_init__test_null_cfg);
    RUN_TEST(test_isotp_init__test_null_can);
    RUN_TEST(test_isotp_init__test_happy_path);
    RUN_TEST(test_isotp_init__test_double_init);
    RUN_TEST(test_isotp_rx_single__test_null_ctx);
    RUN_TEST(test_isotp_rx_single__test_null_frame);
    RUN_TEST(test_isotp_rx_single__test_single_frame_3_bytes);
    RUN_TEST(test_isotp_rx_single__test_sf_zero_length);
    RUN_TEST(test_isotp_rx_single__test_sf_seven_bytes);
    RUN_TEST(test_isotp_rx_multi__test_first_frame_triggers_fc);
    RUN_TEST(test_isotp_rx_multi__test_ff_cf_complete);
    RUN_TEST(test_isotp_rx_multi__test_cf_without_ff);
    RUN_TEST(test_isotp_rx_multi__test_rx_block_size_periodic_fc);
    RUN_TEST(test_isotp_rx_multi__test_rx_block_size_zero_single_fc);
    RUN_TEST(test_isotp_rx_multi__test_ff_fc_cts_tx_failure_is_reported);
    RUN_TEST(test_isotp_rx_multi__test_fc_tx_failure_injection_is_fc_specific);
    RUN_TEST(test_isotp_rx_multi__test_n_ar_armed_across_fc_transmit_only);
    RUN_TEST(test_isotp_rx_multi__test_periodic_block_boundary_fc_tx_failure_is_reported);
#if ISOTP_ENABLE_CAN_FD
    RUN_TEST(test_isotp_rx_multi__test_ff_overflow);
    RUN_TEST(test_isotp_rx_multi__test_ff_overflow_fc_tx_failure_is_reported);
    RUN_TEST(test_isotp_canfd__test_fd_sf_rx_10_bytes);
    RUN_TEST(test_isotp_canfd__test_fd_sf_rx_62_bytes);
    RUN_TEST(test_isotp_canfd__test_fd_sf_rx_zero_dl);
    RUN_TEST(test_isotp_canfd__test_fd_sf_tx_10_bytes);
    RUN_TEST(test_isotp_canfd__test_fd_ff_escape_rx_fits);
    RUN_TEST(test_isotp_canfd__test_fd_ff_escape_rx_overflow);
    RUN_TEST(test_isotp_canfd__test_fd_ff_escape_classic_can_rejected);
    RUN_TEST(test_isotp_canfd__test_fd_ff_escape_tx);
#endif /* ISOTP_ENABLE_CAN_FD */
    RUN_TEST(test_isotp_transmit__test_null_ctx);
    RUN_TEST(test_isotp_transmit__test_null_data);
    RUN_TEST(test_isotp_transmit__test_zero_length);
    RUN_TEST(test_isotp_transmit__test_overflow_length);
    RUN_TEST(test_isotp_transmit__test_single_frame_transmit);
    RUN_TEST(test_isotp_transmit__test_multi_frame_ff);
    RUN_TEST(test_isotp_transmit__test_tx_busy);
    RUN_TEST(test_isotp_tick__test_null_ctx);
    RUN_TEST(test_isotp_tick__test_cr_timeout);
    RUN_TEST(test_isotp_tick__test_tick_idle);
    RUN_TEST(test_isotp_reset__test_null_ctx);
    RUN_TEST(test_isotp_reset__test_reset_from_error);
    RUN_TEST(test_isotp_get_state__test_null_ctx);
    RUN_TEST(test_isotp_get_state__test_null_state_ptr);
#if ISOTP_TX_PADDING
    RUN_TEST(test_isotp_padding__test_classic_sf_padded);
    RUN_TEST(test_isotp_padding__test_classic_sf_max_payload_no_tail);
    RUN_TEST(test_isotp_padding__test_fc_padded);
    RUN_TEST(test_isotp_padding__test_classic_cf_padded);
#if ISOTP_ENABLE_CAN_FD
    RUN_TEST(test_isotp_padding__test_fd_sf_padded_no_tail);
    RUN_TEST(test_isotp_padding__test_fd_sf_padded_one_tail_byte);
#endif /* ISOTP_ENABLE_CAN_FD */
#endif /* ISOTP_TX_PADDING */
}
