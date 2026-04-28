// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_isotp_concurrent.c
 *
 * MODULE UNDER TEST: transport/isotp.c
 *
 * PURPOSE:
 *   Verify ISO-TP behaviour when a new request interrupts an in-progress
 *   multi-frame reassembly. On a live CAN bus, a tester may retransmit or a
 *   second request may arrive before the previous multi-frame sequence
 *   completes. The stack must handle this without corrupting internal state,
 *   without calling the RX completion callback for a partial message, and
 *   without locking up.
 *
 * TEST CASES:
 *   TC-CONC-001  SF mid-multiframe: SF during FF+CF reassembly aborts the
 *                multi-frame and dispatches the SF immediately. The partial
 *                multi-frame data is discarded.
 *
 *   TC-CONC-002  FF mid-multiframe: a new FF while awaiting CFs aborts the
 *                previous multi-frame and starts a new one. The state machine
 *                resets to RX_WAIT_CF for the new FF.
 *
 *   TC-CONC-003  CF without prior FF: a CF frame received while rx_state is
 *                IDLE returns ERR_TP_UNEXPECTED_PDU. No callback is fired.
 *
 *   TC-CONC-004  SF after complete multi-frame: once a multi-frame has been
 *                fully reassembled (state returns to IDLE), a new SF is
 *                processed normally. No state residue from the previous
 *                exchange.
 *
 *   TC-CONC-005  Wrong SN mid-multiframe: a CF with an incorrect sequence
 *                number (e.g. SN=3 when SN=2 expected) transitions to ERROR
 *                state. A subsequent SF recovers correctly (SF resets to IDLE
 *                and dispatches).
 *
 *   TC-CONC-006  N_Cr timeout followed by new SF: after the Cr timer fires
 *                and moves the state to ERROR, a new SF is accepted and
 *                dispatched.
 *
 * FRAMEWORK: Zephyr Ztest (host shim)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "isotp.h"
#include "can_transport.h"
#include "uds_types.h"

/* =========================================================================
 * Mock CAN transport
 * ========================================================================= */

#define MAX_TX_FRAMES (32U)

static uds_can_frame_t g_tx_frames[MAX_TX_FRAMES];
static uint8_t         g_tx_count;
static uds_status_t    g_tx_return;

static uds_status_t mock_tx(can_transport_t *self,
                             const uds_can_frame_t *frame)
{
    (void)self;
    if (g_tx_count < MAX_TX_FRAMES) {
        g_tx_frames[g_tx_count++] = *frame;
    }
    return g_tx_return;
}

static uds_status_t mock_rx(can_transport_t *self,
                             uds_can_frame_t *f, bool *ready)
{
    (void)self; (void)f;
    *ready = false;
    return UDS_STATUS_OK;
}

static uds_status_t mock_status(can_transport_t *self, bool *bus_off)
{
    (void)self;
    *bus_off = false;
    return UDS_STATUS_OK;
}

static const can_transport_ops_t g_mock_ops = {
    .transmit   = mock_tx,
    .receive    = mock_rx,
    .get_status = mock_status,
};

static can_transport_t g_mock_can = {
    .ops      = &g_mock_ops,
    .platform = NULL,
    .ready    = true,
};

/* =========================================================================
 * RX completion tracking
 * ========================================================================= */

#define MAX_CB_DATA (256U)

static uint8_t  g_cb_data[MAX_CB_DATA];
static uint16_t g_cb_len;
static uint8_t  g_cb_count;

static void rx_complete_cb(const uint8_t *data, uint16_t length, void *arg)
{
    (void)arg;
    g_cb_count++;
    if (length <= (uint16_t)MAX_CB_DATA) {
        (void)memcpy(g_cb_data, data, (size_t)length);
    }
    g_cb_len = length;
}

/* =========================================================================
 * Frame builders
 * ========================================================================= */

/** Build a Single Frame with payload bytes b0..b(len-1). */
static uds_can_frame_t make_sf(const uint8_t *payload, uint8_t len)
{
    uds_can_frame_t f;
    (void)memset(&f, 0, sizeof(f));
    f.id             = 0x7DFU;
    f.data[0]        = (uint8_t)(0x00U | (len & (uint8_t)0x0FU));
    (void)memcpy(&f.data[1], payload, (size_t)len);
    f.dlc            = (uint8_t)(1U + len);
    f.is_extended_id = false;
    return f;
}

/** Build a First Frame for a message of total length ff_dl (8–4095). */
static uds_can_frame_t make_ff(uint16_t ff_dl, const uint8_t *first_bytes)
{
    uds_can_frame_t f;
    (void)memset(&f, 0, sizeof(f));
    f.id             = 0x7DFU;
    f.data[0]        = (uint8_t)(0x10U | (uint8_t)((ff_dl >> 8U) & (uint8_t)0x0FU));
    f.data[1]        = (uint8_t)(ff_dl & (uint8_t)0xFFU);
    /* Copy up to 6 first payload bytes. */
    (void)memcpy(&f.data[2], first_bytes, 6U);
    f.dlc            = (uint8_t)8U;
    f.is_extended_id = false;
    return f;
}

/** Build a Consecutive Frame with given SN (0x0–0xF) and payload. */
static uds_can_frame_t make_cf(uint8_t sn, const uint8_t *payload, uint8_t len)
{
    uds_can_frame_t f;
    (void)memset(&f, 0, sizeof(f));
    f.id             = 0x7DFU;
    f.data[0]        = (uint8_t)(0x20U | (sn & (uint8_t)0x0FU));
    if (len > (uint8_t)7U) { len = (uint8_t)7U; }
    (void)memcpy(&f.data[1], payload, (size_t)len);
    f.dlc            = (uint8_t)(1U + len);
    f.is_extended_id = false;
    return f;
}

/* =========================================================================
 * Test fixture
 * ========================================================================= */

static isotp_ctx_t g_ctx;

static void setup(void)
{
    (void)memset(&g_ctx, 0, sizeof(g_ctx));
    g_tx_count    = (uint8_t)0U;
    g_tx_return   = UDS_STATUS_OK;
    g_cb_count    = (uint8_t)0U;
    g_cb_len      = (uint16_t)0U;
    (void)memset(g_cb_data, 0, sizeof(g_cb_data));

    isotp_cfg_t cfg = {
        .can            = &g_mock_can,
        .rx_can_id      = 0x7DFU,
        .tx_can_id      = 0x7E8U,
        .block_size     = (uint8_t)0U,
        .stmin_ms       = (uint8_t)0U,
    };
    (void)isotp_init(&g_ctx, &cfg);
}

ZTEST_SUITE(test_isotp_concurrent, NULL, NULL, NULL, NULL, NULL);

/* =========================================================================
 * TC-CONC-001: SF mid-multiframe
 *
 * Scenario: FF arrives (13-byte message), then instead of a CF the tester
 * sends a new SF (a 3-byte message). The stack must:
 *   - Abort the in-progress multi-frame (discard partial data)
 *   - Dispatch the SF payload immediately via the callback
 *   - Leave rx_state == IDLE after the SF completes
 * ========================================================================= */
ZTEST(test_isotp_concurrent, tc001_sf_interrupts_multiframe)
{
    uds_status_t rc;

    setup();

    /* Start a 13-byte multi-frame. */
    const uint8_t ff_payload[6] = { 0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U };
    uds_can_frame_t ff = make_ff((uint16_t)13U, ff_payload);
    rc = isotp_process_rx_frame(&g_ctx, &ff, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "FF must be accepted");
    zassert_equal(g_cb_count, (uint8_t)0U, "No callback yet after FF");

    /* State must be RX_WAIT_CF. */
    isotp_state_t rx_st; isotp_state_t tx_st;
    rx_st = g_ctx.rx_state; tx_st = g_ctx.tx_state; (void)tx_st;
    zassert_equal(rx_st, ISOTP_STATE_RX_WAIT_CF,
                  "Must be waiting for CF after FF");

    /* Now send a SF (3 bytes) instead of the expected CF. */
    const uint8_t sf_payload[3] = { 0xAAU, 0xBBU, 0xCCU };
    uds_can_frame_t sf = make_sf(sf_payload, (uint8_t)3U);
    rc = isotp_process_rx_frame(&g_ctx, &sf, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "SF must be accepted");

    /* Callback must have fired exactly once — for the SF, not the partial FF. */
    zassert_equal(g_cb_count, (uint8_t)1U,
                  "Callback must fire once for the SF");
    zassert_equal(g_cb_len, (uint16_t)3U,
                  "Callback length must be the SF payload length");
    zassert_equal(g_cb_data[0], (uint8_t)0xAAU, "SF data[0] correct");
    zassert_equal(g_cb_data[1], (uint8_t)0xBBU, "SF data[1] correct");
    zassert_equal(g_cb_data[2], (uint8_t)0xCCU, "SF data[2] correct");

    /* State must be IDLE after SF dispatch. */
    rx_st = g_ctx.rx_state; tx_st = g_ctx.tx_state; (void)tx_st;
    zassert_equal(rx_st, ISOTP_STATE_IDLE,
                  "rx_state must be IDLE after SF dispatch");
}

/* =========================================================================
 * TC-CONC-002: FF mid-multiframe (retransmit / new request)
 *
 * Scenario: FF arrives (13-byte message), then a new FF arrives before any CF.
 * The stack must abort the first multi-frame, start reassembly for the second
 * FF, send a new FC CTS, and leave state in RX_WAIT_CF.
 * ========================================================================= */
ZTEST(test_isotp_concurrent, tc002_ff_restarts_multiframe)
{
    uds_status_t rc;

    setup();

    /* First FF — 13-byte message. */
    const uint8_t ff1_payload[6] = { 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U };
    uds_can_frame_t ff1 = make_ff((uint16_t)13U, ff1_payload);
    rc = isotp_process_rx_frame(&g_ctx, &ff1, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "First FF must be accepted");
    zassert_equal(g_cb_count, (uint8_t)0U, "No callback after first FF");

    uint8_t fc_count_after_first = g_tx_count;
    zassert_true(fc_count_after_first >= (uint8_t)1U,
                 "A FC CTS must be sent after the first FF");

    /* Second FF — different 10-byte message. */
    const uint8_t ff2_payload[6] = { 0xA1U, 0xA2U, 0xA3U, 0xA4U, 0xA5U, 0xA6U };
    uds_can_frame_t ff2 = make_ff((uint16_t)10U, ff2_payload);
    rc = isotp_process_rx_frame(&g_ctx, &ff2, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "Second FF must be accepted");
    zassert_equal(g_cb_count, (uint8_t)0U, "No callback after second FF");

    /* A second FC CTS must have been sent. */
    zassert_true(g_tx_count > fc_count_after_first,
                 "A new FC CTS must be sent after the second FF");

    /* State must be RX_WAIT_CF for the new multi-frame. */
    isotp_state_t rx_st; isotp_state_t tx_st;
    rx_st = g_ctx.rx_state; tx_st = g_ctx.tx_state; (void)tx_st;
    zassert_equal(rx_st, ISOTP_STATE_RX_WAIT_CF,
                  "Must be in RX_WAIT_CF after second FF");

    /* Verify the new reassembly context uses ff2's length. */
    zassert_equal(g_ctx.rx_expected_len, (uint16_t)10U,
                  "rx_expected_len must reflect the second FF");

    /* Send the CF to complete the second multi-frame. */
    const uint8_t cf_payload[4] = { 0xA7U, 0xA8U, 0xA9U, 0xAAU };
    uds_can_frame_t cf = make_cf((uint8_t)1U, cf_payload, (uint8_t)4U);
    rc = isotp_process_rx_frame(&g_ctx, &cf, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "CF must be accepted");
    zassert_equal(g_cb_count, (uint8_t)1U,
                  "Callback must fire once after second multi-frame completes");
    /* First 6 bytes come from FF2, next 4 from the CF. */
    zassert_equal(g_cb_data[0], (uint8_t)0xA1U, "Reassembled data[0]");
    zassert_equal(g_cb_data[6], (uint8_t)0xA7U, "Reassembled data[6]");
}

/* =========================================================================
 * TC-CONC-003: CF received while IDLE (no prior FF)
 *
 * Scenario: A CF arrives when the stack is in IDLE state (no active
 * multi-frame). The stack must reject it with ERR_TP_UNEXPECTED_PDU and
 * not fire the callback.
 * ========================================================================= */
ZTEST(test_isotp_concurrent, tc003_cf_without_ff_rejected)
{
    setup();

    const uint8_t cf_payload[7] = {
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U
    };
    uds_can_frame_t cf = make_cf((uint8_t)1U, cf_payload, (uint8_t)7U);

    uds_status_t rc = isotp_process_rx_frame(&g_ctx, &cf, rx_complete_cb, NULL);

    zassert_equal(rc, UDS_STATUS_ERR_TP_UNEXPECTED_PDU,
                  "CF with no prior FF must return ERR_TP_UNEXPECTED_PDU");
    zassert_equal(g_cb_count, (uint8_t)0U, "No callback must fire");
}

/* =========================================================================
 * TC-CONC-004: SF after complete multi-frame — no state residue
 *
 * Scenario: Complete a 2-frame multi-frame (FF + 1 CF), then send a SF.
 * The SF must be processed as if the context is fresh — no residue.
 * ========================================================================= */
ZTEST(test_isotp_concurrent, tc004_sf_after_completed_multiframe)
{
    uds_status_t rc;

    setup();

    /* Complete a 10-byte multi-frame. */
    const uint8_t ff_payload[6] = { 0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U };
    uds_can_frame_t ff = make_ff((uint16_t)10U, ff_payload);
    rc = isotp_process_rx_frame(&g_ctx, &ff, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "FF accepted");

    const uint8_t cf_payload[4] = { 0x16U, 0x17U, 0x18U, 0x19U };
    uds_can_frame_t cf = make_cf((uint8_t)1U, cf_payload, (uint8_t)4U);
    rc = isotp_process_rx_frame(&g_ctx, &cf, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "CF accepted");
    zassert_equal(g_cb_count, (uint8_t)1U, "Multi-frame callback fired");

    /* State must be IDLE. */
    isotp_state_t rx_st; isotp_state_t tx_st;
    rx_st = g_ctx.rx_state; tx_st = g_ctx.tx_state; (void)tx_st;
    zassert_equal(rx_st, ISOTP_STATE_IDLE,
                  "State must be IDLE after completed multi-frame");

    /* Now send a fresh SF. */
    const uint8_t sf_payload[3] = { 0xDEU, 0xADU, 0xBEU };
    uds_can_frame_t sf = make_sf(sf_payload, (uint8_t)3U);
    rc = isotp_process_rx_frame(&g_ctx, &sf, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "SF after completed multi-frame accepted");
    zassert_equal(g_cb_count, (uint8_t)2U, "SF callback fired");
    zassert_equal(g_cb_len, (uint16_t)3U, "SF length correct");
    zassert_equal(g_cb_data[0], (uint8_t)0xDEU, "SF data[0]");
}

/* =========================================================================
 * TC-CONC-005: Wrong SN mid-multiframe, then recovery with SF
 *
 * Scenario: FF arrives, then a CF with the wrong SN (SN=2 instead of SN=1).
 * The stack transitions to ERROR. A subsequent SF should recover (SF resets
 * the state to IDLE and dispatches normally).
 * ========================================================================= */
ZTEST(test_isotp_concurrent, tc005_wrong_sn_then_sf_recovers)
{
    uds_status_t rc;

    setup();

    /* Start a 13-byte multi-frame. */
    const uint8_t ff_payload[6] = { 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U };
    uds_can_frame_t ff = make_ff((uint16_t)13U, ff_payload);
    rc = isotp_process_rx_frame(&g_ctx, &ff, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "FF accepted");

    /* Send CF with wrong SN — SN=2 when SN=1 expected. */
    const uint8_t bad_cf_payload[7] = {
        0x07U, 0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU
    };
    uds_can_frame_t bad_cf = make_cf((uint8_t)2U, bad_cf_payload, (uint8_t)7U);
    rc = isotp_process_rx_frame(&g_ctx, &bad_cf, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_TP_UNEXPECTED_PDU,
                  "Wrong SN must return ERR_TP_UNEXPECTED_PDU");
    zassert_equal(g_cb_count, (uint8_t)0U, "No callback on wrong SN");

    isotp_state_t rx_st; isotp_state_t tx_st;
    rx_st = g_ctx.rx_state; tx_st = g_ctx.tx_state; (void)tx_st;
    zassert_equal(rx_st, ISOTP_STATE_ERROR,
                  "State must be ERROR after wrong SN");

    /* Recovery: send a fresh SF. ISO-TP spec: SF always resets to IDLE. */
    const uint8_t sf_payload[2] = { 0xF0U, 0xF1U };
    uds_can_frame_t sf = make_sf(sf_payload, (uint8_t)2U);
    rc = isotp_process_rx_frame(&g_ctx, &sf, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "SF after ERROR state accepted");
    zassert_equal(g_cb_count, (uint8_t)1U, "SF callback fired after recovery");
    zassert_equal(g_cb_len, (uint16_t)2U, "SF length after recovery");
    zassert_equal(g_cb_data[0], (uint8_t)0xF0U, "SF data[0] after recovery");

    rx_st = g_ctx.rx_state; tx_st = g_ctx.tx_state; (void)tx_st;
    zassert_equal(rx_st, ISOTP_STATE_IDLE,
                  "State must be IDLE after SF recovery");
}

/* =========================================================================
 * TC-CONC-006: N_Cr timeout then new SF
 *
 * Scenario: FF arrives, then the N_Cr timer expires (simulate by ticking
 * past ISOTP_TIMEOUT_CR_MS). The state transitions to ERROR. A subsequent
 * SF must be accepted and dispatched normally.
 * ========================================================================= */
ZTEST(test_isotp_concurrent, tc006_ncr_timeout_then_sf_recovers)
{
    uds_status_t rc;
    uint32_t     t;

    setup();

    /* Start a 13-byte multi-frame. */
    const uint8_t ff_payload[6] = { 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU };
    uds_can_frame_t ff = make_ff((uint16_t)13U, ff_payload);
    rc = isotp_process_rx_frame(&g_ctx, &ff, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "FF accepted");

    /* Tick past the N_Cr timeout (150 ms per ISOTP_TIMEOUT_CR_MS). */
    for (t = 0U; t < 160U; t++) {
        (void)isotp_tick_1ms(&g_ctx);
    }

    isotp_state_t rx_st; isotp_state_t tx_st;
    rx_st = g_ctx.rx_state; tx_st = g_ctx.tx_state; (void)tx_st;
    zassert_equal(rx_st, ISOTP_STATE_ERROR,
                  "State must be ERROR after N_Cr timeout");
    zassert_equal(g_cb_count, (uint8_t)0U, "No callback on timeout");

    /* Send a fresh SF — must recover. */
    const uint8_t sf_payload[4] = { 0x11U, 0x22U, 0x33U, 0x44U };
    uds_can_frame_t sf = make_sf(sf_payload, (uint8_t)4U);
    rc = isotp_process_rx_frame(&g_ctx, &sf, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_OK, "SF after N_Cr timeout accepted");
    zassert_equal(g_cb_count, (uint8_t)1U, "SF callback fired");
    zassert_equal(g_cb_len, (uint16_t)4U, "SF length correct");

    rx_st = g_ctx.rx_state; tx_st = g_ctx.tx_state; (void)tx_st;
    zassert_equal(rx_st, ISOTP_STATE_IDLE,
                  "State must be IDLE after SF recovery from timeout");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_isotp_concurrent__tc001_sf_interrupts_multiframe(void);
extern void test_isotp_concurrent__tc002_ff_restarts_multiframe(void);
extern void test_isotp_concurrent__tc003_cf_without_ff_rejected(void);
extern void test_isotp_concurrent__tc004_sf_after_completed_multiframe(void);
extern void test_isotp_concurrent__tc005_wrong_sn_then_sf_recovers(void);
extern void test_isotp_concurrent__tc006_ncr_timeout_then_sf_recovers(void);

void run_all_tests(void)
{
    RUN_TEST(test_isotp_concurrent__tc001_sf_interrupts_multiframe);
    RUN_TEST(test_isotp_concurrent__tc002_ff_restarts_multiframe);
    RUN_TEST(test_isotp_concurrent__tc003_cf_without_ff_rejected);
    RUN_TEST(test_isotp_concurrent__tc004_sf_after_completed_multiframe);
    RUN_TEST(test_isotp_concurrent__tc005_wrong_sn_then_sf_recovers);
    RUN_TEST(test_isotp_concurrent__tc006_ncr_timeout_then_sf_recovers);
}
