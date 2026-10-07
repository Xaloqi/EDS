// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_isotp_conformance_vectors.c
 *
 * MODULE UNDER TEST: transport/isotp.c (ISO 15765-2 / ISO-TP).
 *
 * [EDS#361] Independent conformance evidence, not derived from Xaloqi's own
 * tester. Every byte sequence and expected outcome below is PORTED from an
 * independent third-party ISO-TP implementation's own test suite, per the
 * source survey at xaloqi-knowledge/strategy/proposals/
 * 2026-10-06-eds361-conformance-vectors-source-survey.md. This file is
 * deliberately NOT named test_isotp.c (that file already exists at
 * tests/unit_runnable/test_isotp.c and is Xaloqi's own, self-authored suite)
 * precisely so provenance is visible at a glance: the whole value of this
 * file is that nobody at Xaloqi wrote these vectors.
 *
 * -----------------------------------------------------------------------------
 * PROVENANCE
 * -----------------------------------------------------------------------------
 *   Upstream project : pylessard/python-can-isotp
 *   Upstream tag     : v2.0.7
 *   Upstream commit  : 4a672cecb8b5b448ebe7174447efb295550ebb90
 *   Upstream files   : test/test_transport_layer_logic.py
 *                       test/TransportLayerBaseTest.py (make_payload(),
 *                         assert_sent_flow_control() helpers)
 *                       isotp/protocol.py (STmin decode table, cross-checked
 *                         against ISO 15765-2 Table 14 for VEC-STMIN-001)
 *   Upstream licence : MIT, Copyright (c) 2017 Pier-Yves Lessard.
 *
 *     Permission is hereby granted, free of charge, to any person obtaining
 *     a copy of this software and associated documentation files (the
 *     "Software"), to deal in the Software without restriction, including
 *     without limitation the rights to use, copy, modify, merge, publish,
 *     distribute, sublicense, and/or sell copies of the Software, and to
 *     permit persons to whom the Software is furnished to do so, subject to
 *     the following conditions: the above copyright notice and this
 *     permission notice shall be included in all copies or substantial
 *     portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT
 *     WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
 *
 *   What was changed in porting: upstream's tests drive a Python stack
 *   object through queued CAN messages and a stack.process() call; this file
 *   calls EDS's C functions (isotp_process_rx_frame / isotp_transmit /
 *   isotp_tick_1ms) directly with the same byte sequences and compares
 *   against the same expected outcomes. Each test case below cites the exact
 *   upstream Python test function or helper it was ported from. Two vectors
 *   (VEC-RX-MF-006, VEC-RX-MF-007) are deliberately sized to EDS's own
 *   ISOTP_RX_BUF_LEN boundary rather than upstream's configured
 *   max_frame_size, because that parameter has no EDS equivalent at runtime
 *   (EDS's RX buffer is a compile-time constant); the encoding and expected
 *   outcome at that boundary are still exactly upstream's mechanism.
 *
 * -----------------------------------------------------------------------------
 * HONEST SCOPE LIMITS — read before extending this file
 * -----------------------------------------------------------------------------
 *   - Addressing mode: EDS implements ISO 15765-2 Normal addressing only
 *     (isotp_cfg_t has no target-address / address-extension field — routing
 *     is by CAN ID alone). Upstream also exercises Extended and Mixed
 *     addressing (test_addressing_modes.py); those modes have no EDS
 *     counterpart and are correctly absent here, not silently skipped.
 *   - CAN FD Single Frame encoding: upstream's PDU decoder (protocol.py)
 *     accepts a non-escaped nibble-length SF (PCI byte 0x0B for an 11-byte
 *     payload, say) on a CAN FD frame, while warning that ISO 15765-2
 *     actually requires the escape form (0x00 + explicit length byte,
 *     ISO 15765-2:2016 §9.8.1) whenever CAN_DL > 8 — see protocol.py's own
 *     "should be encoded on byte #1" warning. EDS's decoder (isotp.c,
 *     ISOTP_FRAME_TYPE_SF case) requires the escape form unconditionally for
 *     any FD frame and rejects the non-escaped form as overflow. This is
 *     upstream being a lenient receiver on a case its own code flags as
 *     non-compliant to send, not an EDS defect — so that specific byte
 *     pattern is deliberately NOT ported as a conformance vector here; it
 *     would fail against EDS for a reason that reflects a receiver-leniency
 *     choice, not a protocol-mechanics error. The FD FF escape sequence
 *     (ISO 15765-2:2016 §9.8.2, used by VEC-RX-MF-007) is identical in both
 *     implementations and IS ported.
 *   - Reserved STmin values (0x80-0xF0, 0xFA-0xFF): upstream's decoder
 *     rejects these outright (protocol.py raises ValueError). EDS's decoder
 *     (isotp_decode_stmin_ms) treats them as 0 ms, a deliberate fail-safe
 *     default rather than aborting the transfer over one malformed timing
 *     byte. Also a design divergence, not a bug; not ported as a vector for
 *     the same reason as above.
 *   - wftmax (upstream's cap on consecutive FC WAIT frames before aborting):
 *     EDS has no equivalent — isotp_tick_1ms's TX path simply restarts the
 *     Bs timer on each WAIT and retries indefinitely. Not portable; not
 *     ported.
 *   - Per the source survey's own §3: these vectors validate ISO-TP frame
 *     segmentation and timing mechanics only. They say nothing about UDS SID
 *     framing or DoIP (separate, cheaper-last follow-up PRs per the survey's
 *     cost-ordered recommendation) and nothing about DID/DTC/routine content.
 *
 * -----------------------------------------------------------------------------
 * ADR-005 (xaloqi-knowledge decisions/ADR-005-execution-truth-semantics.md)
 * -----------------------------------------------------------------------------
 *   DECLARED FLOOR: all cases in this file, every run. No external
 *   dependency, credential, or optional tier gates any case here (pure host
 *   C, a static mock CAN transport, no network, no license file) — this is
 *   the rare suite ADR-005 itself calls out where 100% executed is the
 *   honest floor rather than an aspirational one. A run that does not
 *   execute every ZTEST case below is BLOCKED, not PASS, and build_tests.sh
 *   has no mechanism to silently skip a C host test module, so this is
 *   enforced by construction rather than by a separate floor check. Per
 *   ADR-005 rule 7, no case count is carried in build_tests.sh's TESTS entry
 *   or in CI job naming for this module.
 *
 * -----------------------------------------------------------------------------
 * lessons/run-014 — "a test that passes against the unfixed code is worth
 * nothing" — compliance note
 * -----------------------------------------------------------------------------
 *   transport/isotp.c already has extensive first-party coverage
 *   (tests/unit_runnable/test_isotp.c, 2000+ lines). Several of the simpler
 *   vectors below (VEC-RX-SF-001, VEC-RX-MF-001) assert the same mechanics
 *   that file already proves and, run against the current, correct
 *   isotp.c, cannot fail for a reason this file alone would catch — their
 *   value is independent SOURCING (provenance), not new defect-finding
 *   coverage, and they are not claimed as the latter. The vectors that ARE
 *   genuinely new failure surface — not duplicated byte-for-byte by any
 *   existing test — were verified the hard way, per this repo's rule: each
 *   was run once against a deliberately broken isotp.c and confirmed to
 *   fail, then re-verified passing after the break was reverted (this
 *   harness's zassert_* macros discard the custom message string and
 *   report only file:line and the two compared values — see
 *   project/unity/unity.h's TEST_ASSERT_EQUAL — so the failures below are
 *   described by what actually printed, not by the discarded message
 *   text; the mutation itself is not kept in git history, it was applied
 *   and reverted locally before this file was committed). The three so
 *   verified:
 *     - VEC-RX-MF-004 (periodic block-boundary FC): changing the #121
 *       block-boundary comparison from >= to > made this fail at the CF3
 *       check (the line asserting g_mock_tx_count==2 after CF3), reporting
 *       the counter still at 1 -- i.e. BS=3's second FC never fired, the
 *       exact #121 behaviour this vector exists to cross-check from an
 *       independent source.
 *     - VEC-RX-MF-006 (4095-byte reassembly, SN nibble wraparound): forcing
 *       the CF handler's SN comparison to ignore the nibble mask made this
 *       fail mid-loop with isotp_process_rx_frame() returning
 *       UDS_STATUS_ERR_TP_UNEXPECTED_PDU (0x36) instead of OK -- by
 *       construction this must be CF #16, the first point the two
 *       counters (one masked, one not) diverge, proving the wraparound is
 *       actually exercised rather than only the final byte-compare.
 *     - VEC-STMIN-001 (sub-millisecond STmin decode): narrowing the decode
 *       range's upper bound by one (0xF9 -> 0xF8) made the 0xF9 case fail
 *       with ctx.tx_stmin_ms reading 0 (the reserved-range fallback)
 *       instead of 1, pinning the upper edge of the range rather than
 *       merely its interior -- the 0xF1 case in the same test still passed.
 *   No case in this file is counted as conformance evidence while only
 *   trivially passing; none were found to do so.
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include "isotp.h"
#include "can_transport.h"
#include "uds_types.h"

/*
 * VEC-RX-MF-006 and VEC-TX-MF-001 below are sized to EDS's default Classic
 * CAN 4095-byte buffer (literal "4095" and fixed-size arrays throughout,
 * not ISOTP_RX_BUF_LEN) to keep the ported byte sequences readable against
 * the upstream source. Caught at compile time, not silently mis-sized, if
 * this build ever overrides the default.
 */
_Static_assert((ISOTP_RX_BUF_LEN) == 4095,
               "[EDS#361] VEC-RX-MF-006/VEC-TX-MF-001 are sized to the "
               "default ISOTP_RX_BUF_LEN=4095; update both if this build "
               "overrides it.");

/* =========================================================================
 * Mock CAN transport — same shape as test_isotp.c's, kept local to this TU
 * on purpose: this file must stand alone as an independently reviewable
 * conformance artifact, not share state with Xaloqi's own suite.
 * ========================================================================= */

#define MOCK_TX_CAPACITY 600U /* [VEC-RX-MF-006/TX-MF-001] >= ceil((4095-6)/7)+1 */

static uds_can_frame_t g_mock_tx_frames[MOCK_TX_CAPACITY];
static uint32_t        g_mock_tx_count;
static uds_status_t    g_mock_tx_return;

static uds_status_t mock_can_transmit(can_transport_t *self, const uds_can_frame_t *frame)
{
    (void)self;
    if (g_mock_tx_count < (uint32_t)MOCK_TX_CAPACITY) {
        g_mock_tx_frames[g_mock_tx_count] = *frame;
    }
    g_mock_tx_count++;
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

static void mock_can_reset(void)
{
    memset(g_mock_tx_frames, 0, sizeof(g_mock_tx_frames));
    g_mock_tx_count  = 0U;
    g_mock_tx_return = UDS_STATUS_OK;
}

/* RX callback state */
static uint8_t  g_rx_cb_data[UDS_MAX_PAYLOAD_LEN];
static uint32_t g_rx_cb_len;
static uint32_t g_rx_cb_call_count;

static void rx_complete_cb(const uint8_t *data, uint32_t length, void *arg)
{
    (void)arg;
    g_rx_cb_call_count++;
    g_rx_cb_len = length;
    if (length <= (uint32_t)UDS_MAX_PAYLOAD_LEN) {
        memcpy(g_rx_cb_data, data, (size_t)length);
    }
}

static void rx_cb_reset(void)
{
    memset(g_rx_cb_data, 0, sizeof(g_rx_cb_data));
    g_rx_cb_len         = 0U;
    g_rx_cb_call_count  = 0U;
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

static uds_can_frame_t make_frame(uint32_t id, const uint8_t *data, uint8_t dlc, bool is_fd)
{
    uds_can_frame_t f;
    memset(&f, 0, sizeof(f));
    f.id    = id;
    f.dlc   = dlc;
    f.is_fd = is_fd;
    if (data != NULL) {
        memcpy(f.data, data, dlc);
    }
    return f;
}

/**
 * Byte-identical port of upstream TransportLayerBaseTest.make_payload():
 *   def make_payload(self, size, start_val=0):
 *       return [int(x % 0x100) for x in range(start_val, start_val + size)]
 */
static void upstream_make_payload(uint8_t *out, uint32_t size, uint32_t start_val)
{
    uint32_t i;
    for (i = 0U; i < size; i++) {
        out[i] = (uint8_t)((start_val + i) % 0x100U);
    }
}

static uds_status_t init_isotp_bs(isotp_ctx_t *ctx, uint8_t block_size, uint8_t stmin_ms)
{
    isotp_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rx_can_id  = 0x222U; /* upstream RXID, test_transport_layer_logic.py */
    cfg.tx_can_id  = 0x111U; /* upstream TXID */
    cfg.block_size = block_size;
    cfg.stmin_ms   = stmin_ms;
#if ISOTP_ENABLE_CAN_FD
    cfg.use_fd     = false;
#endif
    cfg.can        = &g_mock_can;
    return isotp_init(ctx, &cfg);
}

static uds_status_t init_isotp(isotp_ctx_t *ctx)
{
    return init_isotp_bs(ctx, 0U, 0U);
}

#if ISOTP_ENABLE_CAN_FD
static uds_status_t init_isotp_fd(isotp_ctx_t *ctx)
{
    isotp_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rx_can_id  = 0x222U;
    cfg.tx_can_id  = 0x111U;
    cfg.block_size = 0U;
    cfg.stmin_ms   = 0U;
    cfg.use_fd     = true;
    cfg.can        = &g_mock_can;
    return isotp_init(ctx, &cfg);
}
#endif /* ISOTP_ENABLE_CAN_FD */

/** Assert frame @p f's leading bytes match @p expected (ignores any
 * ISOTP_TX_PADDING trailer — padding is an EDS feature orthogonal to the
 * conformance vector being checked). */
static void assert_frame_prefix(const uds_can_frame_t *f, const uint8_t *expected,
                                 uint8_t expected_len, const char *what)
{
    zassert_true(f->dlc >= expected_len, "%s: frame too short", what);
    zassert_equal(memcmp(f->data, expected, (size_t)expected_len), 0,
                  "%s: frame content mismatch", what);
}

/* =========================================================================
 * VEC-RX-SF-001
 * Upstream: test_transport_layer_logic.py::test_receive_single_sf
 *   self.simulate_rx(data=[0x05, 0x11, 0x22, 0x33, 0x44, 0x55])
 *   self.assertEqual(self.rx_isotp_frame(), bytearray([0x11,0x22,0x33,0x44,0x55]))
 * ========================================================================= */

ZTEST_SUITE(test_isotp_conformance_vectors, NULL, NULL, NULL, NULL, NULL);

ZTEST(test_isotp_conformance_vectors, vec_rx_sf_001_single_sf)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t sf[] = { 0x05U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x00U, 0x00U };
    uds_can_frame_t f = make_frame(0x222U, sf, 8U, false);

    zassert_equal(isotp_process_rx_frame(&ctx, &f, rx_complete_cb, NULL), UDS_STATUS_OK,
                  "SF must be accepted");
    zassert_equal(g_rx_cb_call_count, 1U, "callback must fire exactly once");
    zassert_equal(g_rx_cb_len, 5U, "length must be 5");
    uint8_t expected[] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
    zassert_equal(memcmp(g_rx_cb_data, expected, 5U), 0, "payload mismatch");
}

/* =========================================================================
 * VEC-RX-MF-001
 * Upstream: test_transport_layer_logic.py::test_receive_multiframe
 *   payload_size = 10; FF carries 6, CF carries 4.
 * ========================================================================= */

ZTEST(test_isotp_conformance_vectors, vec_rx_mf_001_ff_cf_reassembly)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t payload[10];
    upstream_make_payload(payload, 10U, 0U);

    uint8_t ff[8] = { 0x10U, 0x0AU };
    memcpy(&ff[2], &payload[0], 6U);
    uds_can_frame_t ff_frame = make_frame(0x222U, ff, 8U, false);
    zassert_equal(isotp_process_rx_frame(&ctx, &ff_frame, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "FF must be accepted");
    zassert_equal(g_rx_cb_call_count, 0U, "callback must not fire after FF alone");

    uint8_t cf[8] = { 0x21U };
    memcpy(&cf[1], &payload[6], 4U);
    uds_can_frame_t cf_frame = make_frame(0x222U, cf, 5U, false);
    zassert_equal(isotp_process_rx_frame(&ctx, &cf_frame, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "CF must be accepted");

    zassert_equal(g_rx_cb_call_count, 1U, "callback must fire exactly once");
    zassert_equal(g_rx_cb_len, 10U, "length must be 10");
    zassert_equal(memcmp(g_rx_cb_data, payload, 10U), 0, "reassembled payload mismatch");
}

/* =========================================================================
 * VEC-RX-MF-002
 * Upstream: test_transport_layer_logic.py::test_receive_multiframe_check_flowcontrol
 *   self.stack.params.set('stmin', 0x02); self.stack.params.set('blocksize', 0x05)
 *   self.assert_sent_flow_control(stmin=2, blocksize=5)
 * TransportLayerBaseTest.assert_sent_flow_control encodes:
 *   data = bytearray([0x30 | flowstatus, blocksize, stmin])  (flowstatus=CTS=0)
 * ========================================================================= */

ZTEST(test_isotp_conformance_vectors, vec_rx_mf_002_fc_cts_exact_bytes)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp_bs(&ctx, 5U, 2U), UDS_STATUS_OK, "init failed");

    uint8_t payload[10];
    upstream_make_payload(payload, 10U, 0U);
    uint8_t ff[8] = { 0x10U, 0x0AU };
    memcpy(&ff[2], &payload[0], 6U);
    uds_can_frame_t ff_frame = make_frame(0x222U, ff, 8U, false);

    zassert_equal(isotp_process_rx_frame(&ctx, &ff_frame, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "FF must be accepted");
    zassert_equal(g_mock_tx_count, 1U, "FF must trigger exactly one FC");

    uint8_t expected_fc[] = { 0x30U, 5U, 2U }; /* CTS, BS=5, STmin=2 */
    assert_frame_prefix(&g_mock_tx_frames[0], expected_fc, 3U, "FC after FF");
}

/* =========================================================================
 * VEC-RX-MF-003
 * Upstream: test_transport_layer_logic.py::test_receive_multiframe_bad_seqnum
 *   self.simulate_rx(data=[0x22] + payload[6:10])  # Bad Sequence number
 *   self.assert_error_triggered(isotp.WrongSequenceNumberError)
 * ========================================================================= */

ZTEST(test_isotp_conformance_vectors, vec_rx_mf_003_bad_sequence_number)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t payload[10];
    upstream_make_payload(payload, 10U, 0U);
    uint8_t ff[8] = { 0x10U, 0x0AU };
    memcpy(&ff[2], &payload[0], 6U);
    uds_can_frame_t ff_frame = make_frame(0x222U, ff, 8U, false);
    zassert_equal(isotp_process_rx_frame(&ctx, &ff_frame, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "FF must be accepted");

    uint32_t tx_before = g_mock_tx_count;

    /* Expected SN is 1; upstream sends SN=2. */
    uint8_t bad_cf[8] = { 0x22U };
    memcpy(&bad_cf[1], &payload[6], 4U);
    uds_can_frame_t bad_cf_frame = make_frame(0x222U, bad_cf, 5U, false);
    uds_status_t rc = isotp_process_rx_frame(&ctx, &bad_cf_frame, rx_complete_cb, NULL);

    zassert_equal(rc, UDS_STATUS_ERR_TP_UNEXPECTED_PDU,
                  "wrong sequence number must be rejected");
    zassert_equal(g_rx_cb_call_count, 0U, "callback must not fire");
    zassert_equal(g_mock_tx_count, tx_before, "no further FC may be sent for a bad SN");

    isotp_state_t state;
    zassert_equal(isotp_get_state(&ctx, &state), UDS_STATUS_OK, "get_state failed");
    zassert_equal(state, ISOTP_STATE_ERROR, "RX must enter ERROR on sequence mismatch");
}

/* =========================================================================
 * VEC-RX-MF-004  [independently cross-checks EDS's own #121 feature]
 * Upstream: test_transport_layer_logic.py::test_long_multiframe_2_flow_control
 *   payload_size=30, stmin=5, blocksize=3. FC expected after FF, after CF3,
 *   none after the final CF5 (CF4 in EDS's 0-based-from-FF-6-byte layout
 *   below; upstream's own frame numbering starts at CF1 after FF).
 * ========================================================================= */

ZTEST(test_isotp_conformance_vectors, vec_rx_mf_004_periodic_block_boundary_fc)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp_bs(&ctx, 3U, 5U), UDS_STATUS_OK, "init failed");

    uint8_t payload[30];
    upstream_make_payload(payload, 30U, 0U);

    uint8_t ff[8] = { 0x10U, 0x1EU }; /* FF_DL = 30 */
    memcpy(&ff[2], &payload[0], 6U);
    uds_can_frame_t ff_frame = make_frame(0x222U, ff, 8U, false);
    zassert_equal(isotp_process_rx_frame(&ctx, &ff_frame, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "FF rejected");
    zassert_equal(g_mock_tx_count, 1U, "FF must trigger exactly one FC");
    uint8_t fc0[] = { 0x30U, 3U, 5U };
    assert_frame_prefix(&g_mock_tx_frames[0], fc0, 3U, "FC after FF");

    /* CF1: bytes 6..12. */
    uint8_t cf1[8] = { 0x21U };
    memcpy(&cf1[1], &payload[6], 7U);
    uds_can_frame_t cf1_frame = make_frame(0x222U, cf1, 8U, false);
    zassert_equal(isotp_process_rx_frame(&ctx, &cf1_frame, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "CF1 rejected");
    zassert_equal(g_mock_tx_count, 1U, "no FC mid-block after CF1");

    /* CF2: bytes 13..19. */
    uint8_t cf2[8] = { 0x22U };
    memcpy(&cf2[1], &payload[13], 7U);
    uds_can_frame_t cf2_frame = make_frame(0x222U, cf2, 8U, false);
    zassert_equal(isotp_process_rx_frame(&ctx, &cf2_frame, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "CF2 rejected");
    zassert_equal(g_mock_tx_count, 1U, "no FC mid-block after CF2");

    /* CF3: bytes 20..26 -- closes block 1 (BS=3): a further FC is required. */
    uint8_t cf3[8] = { 0x23U };
    memcpy(&cf3[1], &payload[20], 7U);
    uds_can_frame_t cf3_frame = make_frame(0x222U, cf3, 8U, false);
    zassert_equal(isotp_process_rx_frame(&ctx, &cf3_frame, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "CF3 rejected");
    zassert_equal(g_mock_tx_count, 2U,
                  "ISO 15765-2 9.6.5: a further FC is required after BS=3 CFs");
    assert_frame_prefix(&g_mock_tx_frames[1], fc0, 3U, "FC after CF3");
    zassert_equal(g_rx_cb_call_count, 0U, "PDU not complete yet");

    /* CF4: bytes 27..29 (3 bytes) -- completes the 30-byte PDU; no FC follows. */
    uint8_t cf4[4] = { 0x24U };
    memcpy(&cf4[1], &payload[27], 3U);
    uds_can_frame_t cf4_frame = make_frame(0x222U, cf4, 4U, false);
    zassert_equal(isotp_process_rx_frame(&ctx, &cf4_frame, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "CF4 rejected");
    zassert_equal(g_mock_tx_count, 2U, "no FC may follow the final CF");

    zassert_equal(g_rx_cb_call_count, 1U, "callback must fire once");
    zassert_equal(g_rx_cb_len, 30U, "length must be 30");
    zassert_equal(memcmp(g_rx_cb_data, payload, 30U), 0, "reassembled payload mismatch");
}

/* =========================================================================
 * VEC-RX-MF-005
 * Upstream: test_transport_layer_logic.py::test_long_multiframe_blocksize_zero
 *   blocksize=0 -> unlimited: exactly one FC (the FF's) for the whole PDU.
 * ========================================================================= */

ZTEST(test_isotp_conformance_vectors, vec_rx_mf_005_blocksize_zero_single_fc)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp_bs(&ctx, 0U, 5U), UDS_STATUS_OK, "init failed");

    uint8_t payload[30];
    upstream_make_payload(payload, 30U, 0U);

    uint8_t ff[8] = { 0x10U, 0x1EU };
    memcpy(&ff[2], &payload[0], 6U);
    uds_can_frame_t ff_frame = make_frame(0x222U, ff, 8U, false);
    zassert_equal(isotp_process_rx_frame(&ctx, &ff_frame, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "FF rejected");
    zassert_equal(g_mock_tx_count, 1U, "FF must trigger exactly one FC");
    uint8_t fc0[] = { 0x30U, 0U, 5U };
    assert_frame_prefix(&g_mock_tx_frames[0], fc0, 3U, "FC after FF (BS=0)");

    static const uint8_t offs[4] = { 6U, 13U, 20U, 27U };
    static const uint8_t lens[4] = { 7U, 7U, 7U, 3U };
    uint8_t sn;
    for (sn = 1U; sn <= 4U; sn++) {
        uint8_t cf[8];
        cf[0] = (uint8_t)(0x20U | (sn & 0x0FU));
        memcpy(&cf[1], &payload[offs[sn - 1U]], lens[sn - 1U]);
        uds_can_frame_t cf_frame = make_frame(0x222U, cf, (uint8_t)(lens[sn - 1U] + 1U), false);
        zassert_equal(isotp_process_rx_frame(&ctx, &cf_frame, rx_complete_cb, NULL),
                      UDS_STATUS_OK, "CF rejected");
        zassert_equal(g_mock_tx_count, 1U,
                      "BS=0 is unlimited: no further FC may ever be sent");
    }

    zassert_equal(g_rx_cb_call_count, 1U, "callback must fire once");
    zassert_equal(memcmp(g_rx_cb_data, payload, 30U), 0, "reassembled payload mismatch");
}

/* =========================================================================
 * VEC-RX-MF-006
 * Upstream: test_transport_layer_logic.py::test_receive_4095_multiframe
 *   payload_size = 4095 (Classic CAN's maximum 12-bit FF_DL). FF carries 6,
 *   each CF carries 7; SN is masked to a nibble and wraps 1..15,0,1,...
 * Ported to EDS's own ISOTP_RX_BUF_LEN boundary (4095 by default) rather
 * than an arbitrary size, so this also proves the largest Classic-CAN
 * transfer EDS's default configuration can hold reassembles byte-identical
 * to an independently generated reference payload, with the SN nibble wrap
 * exercised across its 15->0 rollover at least twice.
 * ========================================================================= */

ZTEST(test_isotp_conformance_vectors, vec_rx_mf_006_max_size_reassembly_sn_wrap)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* BS=0: isolate reassembly correctness from block-boundary FC pacing,
     * which VEC-RX-MF-004/005 above already cover from the same source. */
    zassert_equal(init_isotp_bs(&ctx, 0U, 0U), UDS_STATUS_OK, "init failed");

    static uint8_t payload[4095];
    upstream_make_payload(payload, 4095U, 0U);

    uint8_t ff[8] = { 0x1FU, 0xFFU }; /* FF_DL = 4095 = 0xFFF */
    memcpy(&ff[2], &payload[0], 6U);
    uds_can_frame_t ff_frame = make_frame(0x222U, ff, 8U, false);
    zassert_equal(isotp_process_rx_frame(&ctx, &ff_frame, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "FF rejected");

    uint32_t n      = 6U;
    uint32_t seqnum = 1U;
    while (n < 4095U) {
        uint8_t  chunk = (uint8_t)((4095U - n) < 7U ? (4095U - n) : 7U);
        uint8_t  cf[8];
        cf[0] = (uint8_t)(0x20U | (seqnum & 0x0FU));
        memcpy(&cf[1], &payload[n], chunk);
        uds_can_frame_t cf_frame = make_frame(0x222U, cf, (uint8_t)(chunk + 1U), false);
        uds_status_t rc = isotp_process_rx_frame(&ctx, &cf_frame, rx_complete_cb, NULL);
        zassert_equal(rc, UDS_STATUS_OK, "CF rejected during max-size reassembly");
        n += 7U;
        seqnum++;
    }

    zassert_equal(g_rx_cb_call_count, 1U, "callback must fire exactly once");
    zassert_equal(g_rx_cb_len, 4095U, "reassembled length must be 4095");
    zassert_equal(memcmp(g_rx_cb_data, payload, 4095U), 0,
                  "reassembled payload must be byte-identical to the reference");

    /* 4089 bytes after the FF, 7 per CF => 585 CFs => seqnum wraps twice
     * (15->0 at CF #15 and #31 of this run). Sanity-check the wrap happened
     * rather than trusting the loop silently: 585 > 16. */
    zassert_true((seqnum - 1U) > 16U, "must have exercised at least one SN wraparound");
}

/* =========================================================================
 * VEC-RX-MF-007
 * Upstream: test_transport_layer_logic.py::test_receive_overflow_handling_escape_sequence
 *   FF_DL (escape form) > configured max frame size ->
 *   FrameTooLongError + assert_sent_flow_control(stmin=0, blocksize=0,
 *     flowstatus=Overflow)  i.e. FC bytes [0x32, 0x00, 0x00]
 * Ported to the exact ISOTP_RX_BUF_LEN+1 boundary (4096) rather than
 * upstream's arbitrary max_frame_size=32, since EDS's buffer size is a
 * compile-time constant with no runtime equivalent of that parameter.
 * ========================================================================= */

#if ISOTP_ENABLE_CAN_FD
ZTEST(test_isotp_conformance_vectors, vec_rx_mf_007_overflow_fc_exact_bytes)
{
    mock_can_reset();
    rx_cb_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp_fd(&ctx), UDS_STATUS_OK, "init failed");

    /* FD escape FF: bytes 0-1 = 0x10,0x00; bytes 2-5 = FF_DL=4096 (0x1000),
     * big-endian; bytes 6-7 = first 2 data bytes (ISO 15765-2:2016 §9.8.2). */
    uint8_t ff[8] = {
        0x10U, 0x00U,
        0x00U, 0x00U, 0x10U, 0x00U,
        0xAAU, 0xBBU
    };
    uds_can_frame_t ff_frame = make_frame(0x222U, ff, 8U, true);

    uds_status_t rc = isotp_process_rx_frame(&ctx, &ff_frame, rx_complete_cb, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_TP_OVERFLOW,
                  "FF_DL = RX_BUF_LEN+1 must be reported as overflow");
    zassert_equal(g_mock_tx_count, 1U, "FC OVFLW must have been sent");

    uint8_t expected_fc[] = { 0x32U, 0x00U, 0x00U }; /* OVFLW, BS=0, STmin=0 */
    assert_frame_prefix(&g_mock_tx_frames[0], expected_fc, 3U, "FC OVFLW");
    zassert_equal(g_rx_cb_call_count, 0U, "no callback for a rejected PDU");
}
#endif /* ISOTP_ENABLE_CAN_FD */

/* =========================================================================
 * VEC-TX-SF-001
 * Upstream: test_transport_layer_logic.py::test_send_single_frame_after_empty_payload
 *   self.tx_isotp_frame([0x55]); self.assertEqual(msg.data, bytearray([0x01,0x55]))
 * (Upstream's surrounding empty-payload sends have no EDS equivalent --
 * isotp_transmit(ctx, data, 0) is itself an invalid-parameter call in EDS,
 * already covered by test_isotp.c's own NULL/zero-length guards; only the
 * single real transmit is portable.)
 * ========================================================================= */

ZTEST(test_isotp_conformance_vectors, vec_tx_sf_001_single_byte)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t data[] = { 0x55U };
    zassert_equal(isotp_transmit(&ctx, data, 1U), UDS_STATUS_OK, "transmit failed");
    zassert_equal(g_mock_tx_count, 1U, "exactly one frame must be sent");

    uint8_t expected[] = { 0x01U, 0x55U };
    assert_frame_prefix(&g_mock_tx_frames[0], expected, 2U, "SF TX");
}

/* =========================================================================
 * VEC-TX-MF-001
 * Upstream: test_transport_layer_logic.py::test_send_blocksize_zero
 *   FF content: bytearray([0x1F, 0xFF] + payload[:6])
 *   Each CF: bytearray([0x20 | seqnum] + payload[n:n+7]), seqnum wraps & 0xF,
 *   blocksize=0 -> every CF sent without waiting for another FC.
 * Mirrors VEC-RX-MF-006 on the transmit side: full 4095-byte Classic-CAN
 * transfer, SN nibble wraparound exercised on TX.
 * ========================================================================= */

ZTEST(test_isotp_conformance_vectors, vec_tx_mf_001_max_size_tx_sn_wrap)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    static uint8_t payload[4095];
    upstream_make_payload(payload, 4095U, 0U);

    zassert_equal(isotp_transmit(&ctx, payload, 4095U), UDS_STATUS_OK, "transmit failed");
    zassert_equal(g_mock_tx_count, 1U, "FF must be the only frame sent before FC");

    uint8_t expected_ff[8] = { 0x1FU, 0xFFU };
    memcpy(&expected_ff[2], &payload[0], 6U);
    assert_frame_prefix(&g_mock_tx_frames[0], expected_ff, 8U, "TX FF");

    /* Grant CTS, BS=0, STmin=0 -- the sender must now pump every CF without
     * waiting for a second FC. */
    uint8_t fc[3] = { 0x30U, 0x00U, 0x00U };
    uds_can_frame_t fc_frame = make_frame(0x111U, fc, 3U, false);
    zassert_equal(isotp_process_rx_frame(&ctx, &fc_frame, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "FC CTS must be accepted");

    uint32_t  tick;
    isotp_state_t state = ISOTP_STATE_TX_SEND_CF;
    for (tick = 0U; (tick < 600U) && (state != ISOTP_STATE_IDLE); tick++) {
        zassert_equal(isotp_tick_1ms(&ctx), UDS_STATUS_OK, "tick must not time out");
        zassert_equal(isotp_get_tx_state(&ctx, &state), UDS_STATUS_OK, "get_tx_state failed");
    }
    zassert_equal(state, ISOTP_STATE_IDLE, "TX must complete within 600 ticks");

    /* 4089 bytes after the FF, 7 per CF => 585 CFs. */
    zassert_equal(g_mock_tx_count, 586U, "FF + 585 CFs expected");

    uint32_t n      = 6U;
    uint32_t seqnum = 1U;
    uint32_t idx    = 1U;
    while (n < 4095U) {
        uint8_t chunk = (uint8_t)((4095U - n) < 7U ? (4095U - n) : 7U);
        uint8_t expected_pci = (uint8_t)(0x20U | (seqnum & 0x0FU));
        zassert_equal(g_mock_tx_frames[idx].data[0], expected_pci,
                      "CF PCI/SN mismatch at CF index %u", idx);
        zassert_equal(memcmp(&g_mock_tx_frames[idx].data[1], &payload[n], chunk), 0,
                      "CF payload mismatch at CF index %u", idx);
        n += 7U;
        seqnum++;
        idx++;
    }
    zassert_true((seqnum - 1U) > 16U, "must have exercised at least one SN wraparound");
}

/* =========================================================================
 * VEC-STMIN-001
 * Upstream: isotp/protocol.py PDU.__init__ STmin decode:
 *   elif stmin_temp >= 0xf1 and stmin_temp <= 0xF9:
 *       self.stmin_sec = (stmin_temp - 0xF0) / 10000   # 100-900 us
 * Cross-checked against ISO 15765-2 Table 14, which isotp.c's own
 * isotp_decode_stmin_ms() already cites. Exercised indirectly through the
 * public API (isotp_decode_stmin_ms is static) by observing ctx.tx_stmin_ms
 * after a Flow Control frame is processed. Both ends of upstream's
 * confirmed range are checked (0xF1 and 0xF9), not an arbitrary interior
 * point, so a one-off error at either edge is caught.
 * ========================================================================= */

static void assert_stmin_rounds_to_1ms(uint8_t stmin_raw, const char *what)
{
    mock_can_reset();
    isotp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(init_isotp(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t tx_payload[20];
    upstream_make_payload(tx_payload, 20U, 0U);
    zassert_equal(isotp_transmit(&ctx, tx_payload, 20U), UDS_STATUS_OK, "transmit failed");

    uint8_t fc[3] = { 0x30U, 0x00U, stmin_raw };
    uds_can_frame_t fc_frame = make_frame(0x111U, fc, 3U, false);
    zassert_equal(isotp_process_rx_frame(&ctx, &fc_frame, rx_complete_cb, NULL),
                  UDS_STATUS_OK, "FC CTS must be accepted");

    zassert_equal(ctx.tx_stmin_ms, 1U, "%s must round up to EDS's minimum 1 ms tick", what);
}

ZTEST(test_isotp_conformance_vectors, vec_stmin_001_sub_millisecond_rounds_up)
{
    assert_stmin_rounds_to_1ms(0xF1U, "0xF1 (100 us, lower edge of the sub-ms range)");
    assert_stmin_rounds_to_1ms(0xF9U, "0xF9 (900 us, upper edge of the sub-ms range)");
}

/* run_all_tests shim */
extern void test_isotp_conformance_vectors__vec_rx_sf_001_single_sf(void);
extern void test_isotp_conformance_vectors__vec_rx_mf_001_ff_cf_reassembly(void);
extern void test_isotp_conformance_vectors__vec_rx_mf_002_fc_cts_exact_bytes(void);
extern void test_isotp_conformance_vectors__vec_rx_mf_003_bad_sequence_number(void);
extern void test_isotp_conformance_vectors__vec_rx_mf_004_periodic_block_boundary_fc(void);
extern void test_isotp_conformance_vectors__vec_rx_mf_005_blocksize_zero_single_fc(void);
extern void test_isotp_conformance_vectors__vec_rx_mf_006_max_size_reassembly_sn_wrap(void);
#if ISOTP_ENABLE_CAN_FD
extern void test_isotp_conformance_vectors__vec_rx_mf_007_overflow_fc_exact_bytes(void);
#endif
extern void test_isotp_conformance_vectors__vec_tx_sf_001_single_byte(void);
extern void test_isotp_conformance_vectors__vec_tx_mf_001_max_size_tx_sn_wrap(void);
extern void test_isotp_conformance_vectors__vec_stmin_001_sub_millisecond_rounds_up(void);

void run_all_tests(void)
{
    RUN_TEST(test_isotp_conformance_vectors__vec_rx_sf_001_single_sf);
    RUN_TEST(test_isotp_conformance_vectors__vec_rx_mf_001_ff_cf_reassembly);
    RUN_TEST(test_isotp_conformance_vectors__vec_rx_mf_002_fc_cts_exact_bytes);
    RUN_TEST(test_isotp_conformance_vectors__vec_rx_mf_003_bad_sequence_number);
    RUN_TEST(test_isotp_conformance_vectors__vec_rx_mf_004_periodic_block_boundary_fc);
    RUN_TEST(test_isotp_conformance_vectors__vec_rx_mf_005_blocksize_zero_single_fc);
    RUN_TEST(test_isotp_conformance_vectors__vec_rx_mf_006_max_size_reassembly_sn_wrap);
#if ISOTP_ENABLE_CAN_FD
    RUN_TEST(test_isotp_conformance_vectors__vec_rx_mf_007_overflow_fc_exact_bytes);
#endif
    RUN_TEST(test_isotp_conformance_vectors__vec_tx_sf_001_single_byte);
    RUN_TEST(test_isotp_conformance_vectors__vec_tx_mf_001_max_size_tx_sn_wrap);
    RUN_TEST(test_isotp_conformance_vectors__vec_stmin_001_sub_millisecond_rounds_up);
}
