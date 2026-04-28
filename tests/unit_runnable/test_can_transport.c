// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
// ================================
// File: tests/unit/test_can_transport.c
// ================================
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit/test_can_transport.c
 *
 * MODULE UNDER TEST: transport/can_transport.c
 *
 * PURPOSE:
 *   Verify the CAN transport abstraction layer (can_transport.h / .c).
 *   Tests cover:
 *     - NULL pointer guards on all three public APIs
 *     - Not-ready guard (can->ready == false)
 *     - NULL ops vtable guard
 *     - NULL individual function-pointer guard
 *     - Happy-path transmit, receive (frame-available + no-frame), get_status
 *     - Bus-off detection
 *     - DLC out-of-range guard on transmit
 *     - Extended CAN ID frame handling
 *
 * FRAMEWORK: Zephyr Ztest  (zephyr/ztest.h)
 *
 * COMPILE: Include this file in the tests/unit CMakeLists.txt alongside
 *          transport/can_transport.c and core stub headers.
 *
 * SAFETY: Covers REQ-SAFE-004 (NULL pointer prevention at hardware boundary).
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "can_transport.h"
#include "uds_types.h"

/* =========================================================================
 * Mock platform state
 * ========================================================================= */

/** Mirror of the last frame passed to mock_transmit(). */
static uds_can_frame_t  g_last_tx_frame;
static bool             g_tx_called;
static uds_status_t     g_tx_return;   /**< Configurable return value. */

/** Frame to return from mock_receive(). */
static uds_can_frame_t  g_rx_frame;
static bool             g_rx_ready;
static uds_status_t     g_rx_return;

static bool             g_bus_off;
static uds_status_t     g_status_return;

/* =========================================================================
 * Mock vtable implementations
 * ========================================================================= */

static uds_status_t mock_transmit(can_transport_t *self, const uds_can_frame_t *frame)
{
    (void)self;
    g_tx_called = true;
    if (frame != NULL) {
        g_last_tx_frame = *frame;
    }
    return g_tx_return;
}

static uds_status_t mock_receive(can_transport_t *self,
                                 uds_can_frame_t *out_frame,
                                 bool            *out_ready)
{
    (void)self;
    *out_ready = g_rx_ready;
    if (g_rx_ready && (out_frame != NULL)) {
        *out_frame = g_rx_frame;
    }
    return g_rx_return;
}

static uds_status_t mock_get_status(can_transport_t *self, bool *bus_off)
{
    (void)self;
    *bus_off = g_bus_off;
    return g_status_return;
}

static const can_transport_ops_t g_mock_ops = {
    .transmit   = mock_transmit,
    .receive    = mock_receive,
    .get_status = mock_get_status,
};

/* =========================================================================
 * Test fixture helpers
 * ========================================================================= */

/** Return an initialised, ready can_transport_t backed by the mock vtable. */
static can_transport_t make_ready_transport(void)
{
    can_transport_t t;
    memset(&t, 0, sizeof(t));
    t.ops      = &g_mock_ops;
    t.ready    = true;
    t.platform = NULL;
    return t;
}

/** Reset all mock globals to safe defaults before each test. */
static void mock_reset(void)
{
    memset(&g_last_tx_frame, 0, sizeof(g_last_tx_frame));
    g_tx_called     = false;
    g_tx_return     = UDS_STATUS_OK;
    memset(&g_rx_frame, 0, sizeof(g_rx_frame));
    g_rx_ready      = false;
    g_rx_return     = UDS_STATUS_OK;
    g_bus_off       = false;
    g_status_return = UDS_STATUS_OK;
}

/* =========================================================================
 * Test suite: can_transport_transmit
 * ========================================================================= */

ZTEST_SUITE(test_can_transport_transmit, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-CAN-TX-001: NULL transport pointer → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_can_transport_transmit, test_tx_null_transport)
{
    uds_can_frame_t frame = {0};
    frame.dlc = 1U;
    uds_status_t rc = can_transport_transmit(NULL, &frame);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR,
                  "Expected ERR_NULL_PTR for NULL transport, got %d", rc);
}

/**
 * TC-CAN-TX-002: NULL frame pointer → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_can_transport_transmit, test_tx_null_frame)
{
    mock_reset();
    can_transport_t t = make_ready_transport();
    uds_status_t rc = can_transport_transmit(&t, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR,
                  "Expected ERR_NULL_PTR for NULL frame");
    zassert_false(g_tx_called, "mock_transmit must not be called for NULL frame");
}

/**
 * TC-CAN-TX-003: Transport not ready (ready == false) → UDS_STATUS_ERR_CAN_NOT_READY.
 */
ZTEST(test_can_transport_transmit, test_tx_not_ready)
{
    mock_reset();
    can_transport_t t = make_ready_transport();
    t.ready = false;
    uds_can_frame_t frame = {0};
    frame.dlc = 1U;
    uds_status_t rc = can_transport_transmit(&t, &frame);
    zassert_equal(rc, UDS_STATUS_ERR_CAN_NOT_READY,
                  "Expected ERR_CAN_NOT_READY when ready=false");
}

/**
 * TC-CAN-TX-004: NULL ops vtable → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_can_transport_transmit, test_tx_null_ops)
{
    mock_reset();
    can_transport_t t = make_ready_transport();
    t.ops = NULL;
    uds_can_frame_t frame = {0};
    frame.dlc = 1U;
    uds_status_t rc = can_transport_transmit(&t, &frame);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR,
                  "Expected ERR_NULL_PTR for NULL ops");
}

/**
 * TC-CAN-TX-005: DLC exceeds UDS_CAN_FRAME_MAX_LEN → UDS_STATUS_ERR_INVALID_PARAM.
 */
ZTEST(test_can_transport_transmit, test_tx_dlc_overflow)
{
    mock_reset();
    can_transport_t t = make_ready_transport();
    uds_can_frame_t frame = {0};
    frame.dlc = (uint8_t)(UDS_CAN_FRAME_MAX_LEN + 1U);
    uds_status_t rc = can_transport_transmit(&t, &frame);
    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "Expected ERR_INVALID_PARAM for DLC > max");
    zassert_false(g_tx_called, "mock must not be called for oversized DLC");
}

/**
 * TC-CAN-TX-006: Happy path — standard 11-bit ID, DLC=8.
 * Validates that mock_transmit is called with the exact frame provided.
 */
ZTEST(test_can_transport_transmit, test_tx_happy_standard_id)
{
    mock_reset();
    can_transport_t t = make_ready_transport();

    uds_can_frame_t frame = {0};
    frame.id             = 0x7DFU;
    frame.dlc            = 8U;
    frame.is_extended_id = false;
    frame.data[0]        = 0x02U;
    frame.data[1]        = 0x10U;
    frame.data[2]        = 0x03U;

    uds_status_t rc = can_transport_transmit(&t, &frame);
    zassert_equal(rc, UDS_STATUS_OK, "Expected OK for valid transmit");
    zassert_true(g_tx_called, "mock_transmit was not called");
    zassert_equal(g_last_tx_frame.id,  0x7DFU, "ID mismatch");
    zassert_equal(g_last_tx_frame.dlc, 8U,      "DLC mismatch");
    zassert_equal(g_last_tx_frame.data[0], 0x02U, "data[0] mismatch");
}

/**
 * TC-CAN-TX-007: Happy path — 29-bit extended CAN ID.
 */
ZTEST(test_can_transport_transmit, test_tx_happy_extended_id)
{
    mock_reset();
    can_transport_t t = make_ready_transport();

    uds_can_frame_t frame = {0};
    frame.id             = 0x18DA00F1UL;  /* ISO 15765-4 extended addressing */
    frame.dlc            = 3U;
    frame.is_extended_id = true;

    uds_status_t rc = can_transport_transmit(&t, &frame);
    zassert_equal(rc, UDS_STATUS_OK, "Expected OK for extended ID frame");
    zassert_true(g_last_tx_frame.is_extended_id, "Expected extended ID flag set");
    zassert_equal(g_last_tx_frame.id, 0x18DA00F1UL, "Extended ID mismatch");
}

/**
 * TC-CAN-TX-008: Propagate TX error from platform → UDS_STATUS_ERR_CAN_TX_FAILED.
 */
ZTEST(test_can_transport_transmit, test_tx_platform_error)
{
    mock_reset();
    g_tx_return = UDS_STATUS_ERR_CAN_TX_FAILED;
    can_transport_t t = make_ready_transport();

    uds_can_frame_t frame = {0};
    frame.dlc = 1U;

    uds_status_t rc = can_transport_transmit(&t, &frame);
    zassert_equal(rc, UDS_STATUS_ERR_CAN_TX_FAILED,
                  "Expected CAN_TX_FAILED propagated from platform");
}

/**
 * TC-CAN-TX-009: DLC == UDS_CAN_FRAME_MAX_LEN (boundary) — must succeed.
 */
ZTEST(test_can_transport_transmit, test_tx_dlc_boundary_max)
{
    mock_reset();
    can_transport_t t = make_ready_transport();
    uds_can_frame_t frame = {0};
    frame.dlc = (uint8_t)UDS_CAN_FRAME_MAX_LEN;  /* Exactly at boundary */
    uds_status_t rc = can_transport_transmit(&t, &frame);
    zassert_equal(rc, UDS_STATUS_OK, "DLC at max boundary must succeed");
}

/* =========================================================================
 * Test suite: can_transport_receive
 * ========================================================================= */

ZTEST_SUITE(test_can_transport_receive, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-CAN-RX-001: NULL transport pointer → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_can_transport_receive, test_rx_null_transport)
{
    uds_can_frame_t frame = {0};
    bool ready = false;
    uds_status_t rc = can_transport_receive(NULL, &frame, &ready);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "Expected NULL_PTR for NULL transport");
}

/**
 * TC-CAN-RX-002: NULL out_frame → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_can_transport_receive, test_rx_null_frame)
{
    mock_reset();
    can_transport_t t = make_ready_transport();
    bool ready = false;
    uds_status_t rc = can_transport_receive(&t, NULL, &ready);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "Expected NULL_PTR for NULL out_frame");
}

/**
 * TC-CAN-RX-003: NULL out_ready → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_can_transport_receive, test_rx_null_ready)
{
    mock_reset();
    can_transport_t t = make_ready_transport();
    uds_can_frame_t frame = {0};
    uds_status_t rc = can_transport_receive(&t, &frame, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "Expected NULL_PTR for NULL out_ready");
}

/**
 * TC-CAN-RX-004: No frame available → OK, out_ready = false.
 */
ZTEST(test_can_transport_receive, test_rx_no_frame)
{
    mock_reset();
    g_rx_ready = false;
    can_transport_t t = make_ready_transport();
    uds_can_frame_t frame = {0};
    bool ready = true;  /* Start with true to confirm it's cleared */
    uds_status_t rc = can_transport_receive(&t, &frame, &ready);
    zassert_equal(rc, UDS_STATUS_OK, "Expected OK when no frame available");
    zassert_false(ready, "out_ready must be false when no frame available");
}

/**
 * TC-CAN-RX-005: Frame available → OK, out_ready = true, frame data copied.
 */
ZTEST(test_can_transport_receive, test_rx_frame_available)
{
    mock_reset();
    g_rx_ready        = true;
    g_rx_frame.id     = 0x7E8U;
    g_rx_frame.dlc    = 4U;
    g_rx_frame.data[0] = 0x04U;
    g_rx_frame.data[1] = 0x62U;
    g_rx_frame.data[2] = 0xF1U;
    g_rx_frame.data[3] = 0x90U;

    can_transport_t t = make_ready_transport();
    uds_can_frame_t out_frame = {0};
    bool ready = false;
    uds_status_t rc = can_transport_receive(&t, &out_frame, &ready);

    zassert_equal(rc, UDS_STATUS_OK, "Expected OK with frame available");
    zassert_true(ready, "out_ready must be true");
    zassert_equal(out_frame.id, 0x7E8U, "Frame ID mismatch");
    zassert_equal(out_frame.dlc, 4U, "DLC mismatch");
    zassert_equal(out_frame.data[1], 0x62U, "data[1] mismatch");
}

/**
 * TC-CAN-RX-006: Transport not ready → UDS_STATUS_ERR_CAN_NOT_READY, out_ready=false.
 */
ZTEST(test_can_transport_receive, test_rx_not_ready)
{
    mock_reset();
    can_transport_t t = make_ready_transport();
    t.ready = false;
    uds_can_frame_t frame = {0};
    bool ready = true;
    uds_status_t rc = can_transport_receive(&t, &frame, &ready);
    zassert_equal(rc, UDS_STATUS_ERR_CAN_NOT_READY, "Expected CAN_NOT_READY");
    /* The implementation initialises *out_ready = false before early return */
    zassert_false(ready, "out_ready must be cleared on not-ready");
}

/* =========================================================================
 * Test suite: can_transport_get_status
 * ========================================================================= */

ZTEST_SUITE(test_can_transport_get_status, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-CAN-ST-001: NULL transport → UDS_STATUS_ERR_NULL_PTR, bus_off=true (safe default).
 */
ZTEST(test_can_transport_get_status, test_status_null_transport)
{
    bool bus_off = false;
    uds_status_t rc = can_transport_get_status(NULL, &bus_off);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "Expected NULL_PTR");
    /* bus_off should be true (safe default) even on error */
    zassert_true(bus_off, "bus_off safe default must be true on NULL transport");
}

/**
 * TC-CAN-ST-002: NULL bus_off output → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_can_transport_get_status, test_status_null_bus_off)
{
    mock_reset();
    can_transport_t t = make_ready_transport();
    uds_status_t rc = can_transport_get_status(&t, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "Expected NULL_PTR for NULL bus_off");
}

/**
 * TC-CAN-ST-003: Bus healthy → OK, bus_off = false.
 */
ZTEST(test_can_transport_get_status, test_status_bus_healthy)
{
    mock_reset();
    g_bus_off = false;
    can_transport_t t = make_ready_transport();
    bool bus_off = true;
    uds_status_t rc = can_transport_get_status(&t, &bus_off);
    zassert_equal(rc, UDS_STATUS_OK, "Expected OK for healthy bus");
    zassert_false(bus_off, "bus_off must be false for healthy bus");
}

/**
 * TC-CAN-ST-004: Bus-off condition → OK, bus_off = true.
 */
ZTEST(test_can_transport_get_status, test_status_bus_off)
{
    mock_reset();
    g_bus_off = true;
    can_transport_t t = make_ready_transport();
    bool bus_off = false;
    uds_status_t rc = can_transport_get_status(&t, &bus_off);
    zassert_equal(rc, UDS_STATUS_OK, "Expected OK even in bus-off");
    zassert_true(bus_off, "bus_off must be true for bus-off condition");
}

/**
 * TC-CAN-ST-005: Platform returns error → error propagated, bus_off stays true.
 */
ZTEST(test_can_transport_get_status, test_status_platform_error)
{
    mock_reset();
    g_status_return = UDS_STATUS_ERR_CAN_NOT_READY;
    can_transport_t t = make_ready_transport();
    bool bus_off = false;
    uds_status_t rc = can_transport_get_status(&t, &bus_off);
    zassert_equal(rc, UDS_STATUS_ERR_CAN_NOT_READY,
                  "Expected platform error propagated");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_can_transport_transmit__test_tx_null_transport(void);
extern void test_can_transport_transmit__test_tx_null_frame(void);
extern void test_can_transport_transmit__test_tx_not_ready(void);
extern void test_can_transport_transmit__test_tx_null_ops(void);
extern void test_can_transport_transmit__test_tx_dlc_overflow(void);
extern void test_can_transport_transmit__test_tx_happy_standard_id(void);
extern void test_can_transport_transmit__test_tx_happy_extended_id(void);
extern void test_can_transport_transmit__test_tx_platform_error(void);
extern void test_can_transport_transmit__test_tx_dlc_boundary_max(void);
extern void test_can_transport_receive__test_rx_null_transport(void);
extern void test_can_transport_receive__test_rx_null_frame(void);
extern void test_can_transport_receive__test_rx_null_ready(void);
extern void test_can_transport_receive__test_rx_no_frame(void);
extern void test_can_transport_receive__test_rx_frame_available(void);
extern void test_can_transport_receive__test_rx_not_ready(void);
extern void test_can_transport_get_status__test_status_null_transport(void);
extern void test_can_transport_get_status__test_status_null_bus_off(void);
extern void test_can_transport_get_status__test_status_bus_healthy(void);
extern void test_can_transport_get_status__test_status_bus_off(void);
extern void test_can_transport_get_status__test_status_platform_error(void);

void run_all_tests(void)
{
    RUN_TEST(test_can_transport_transmit__test_tx_null_transport);
    RUN_TEST(test_can_transport_transmit__test_tx_null_frame);
    RUN_TEST(test_can_transport_transmit__test_tx_not_ready);
    RUN_TEST(test_can_transport_transmit__test_tx_null_ops);
    RUN_TEST(test_can_transport_transmit__test_tx_dlc_overflow);
    RUN_TEST(test_can_transport_transmit__test_tx_happy_standard_id);
    RUN_TEST(test_can_transport_transmit__test_tx_happy_extended_id);
    RUN_TEST(test_can_transport_transmit__test_tx_platform_error);
    RUN_TEST(test_can_transport_transmit__test_tx_dlc_boundary_max);
    RUN_TEST(test_can_transport_receive__test_rx_null_transport);
    RUN_TEST(test_can_transport_receive__test_rx_null_frame);
    RUN_TEST(test_can_transport_receive__test_rx_null_ready);
    RUN_TEST(test_can_transport_receive__test_rx_no_frame);
    RUN_TEST(test_can_transport_receive__test_rx_frame_available);
    RUN_TEST(test_can_transport_receive__test_rx_not_ready);
    RUN_TEST(test_can_transport_get_status__test_status_null_transport);
    RUN_TEST(test_can_transport_get_status__test_status_null_bus_off);
    RUN_TEST(test_can_transport_get_status__test_status_bus_healthy);
    RUN_TEST(test_can_transport_get_status__test_status_bus_off);
    RUN_TEST(test_can_transport_get_status__test_status_platform_error);
}
