// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_uds_periodic.c
 *
 * MODULE UNDER TEST: core/uds_periodic.c
 *                    SID 0x2A — ReadDataByPeriodicIdentifier scheduler
 *
 * PURPOSE:
 *   Verify the scheduler's subscription lifecycle, timer mechanics, and
 *   frame-pop behaviour. These tests exercise the module without the full
 *   UDS service dispatcher — the handler under test (service_0x2A.c) is
 *   covered in a separate suite.
 *
 *   A test DID (0xF2AB, 2 bytes, read-only) is registered once in the
 *   DID database so that uds_periodic_pop_due() can exercise the full
 *   happy path including the data-read callback.
 *
 * TEST CASES:
 *   TC-PERIODIC-001  uds_periodic_init() returns UDS_STATUS_OK
 *   TC-PERIODIC-002  subscribe(0xAB, SLOW) → OK; no ticks → pop_due NOT_FOUND
 *   TC-PERIODIC-003  re-subscribe same ID → updates in-place (slot count stays 1)
 *   TC-PERIODIC-004  subscribe up to UDS_PERIODIC_MAX_SUBSCRIPTIONS → all OK
 *   TC-PERIODIC-005  subscribe beyond max → ERR_REQUEST_OUT_OF_RANGE
 *   TC-PERIODIC-006  unsubscribe known ID → OK; slot reclaimed for new subscribe
 *   TC-PERIODIC-007  unsubscribe unknown ID → OK (no-op)
 *   TC-PERIODIC-008  cancel_all() → pop_due returns ERR_NOT_FOUND
 *   TC-PERIODIC-009  tick_1ms() returns UDS_STATUS_OK
 *   TC-PERIODIC-010  FAST (10 ms): after 10 ticks → pop_due returns OK
 *   TC-PERIODIC-011  FAST (10 ms): after 9 ticks  → pop_due returns ERR_NOT_FOUND
 *   TC-PERIODIC-012  pop_due(NULL) → ERR_NULL_PTR
 *   TC-PERIODIC-013  pop_due with no subscriptions → ERR_NOT_FOUND
 *   TC-PERIODIC-014  frame format: data[0]=0x6A, data[1]=periodicId after FAST fires
 *
 * FRAMEWORK: Zephyr Ztest (compiled as host Unity via ztest_shim.h)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "uds_periodic.h"
#include "uds_types.h"
#include "did_database.h"

/* =========================================================================
 * Test DID: 0xF2AB (periodic identifier 0xAB)
 * ========================================================================= */

static uds_status_t t_read_f2ab(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    (void)buf_len;
    buf[0]   = 0xCAU;
    buf[1]   = 0xFEU;
    *out_len = (uint16_t)2U;
    return UDS_STATUS_OK;
}

/* =========================================================================
 * One-time setup (DID database must be initialised exactly once per binary)
 * ========================================================================= */

static bool g_db_ready = false;

static void setup(void)
{
    if (!g_db_ready) {
        (void)did_database_init();

        static const did_entry_t s_test_did = {
            .did_id             = (uint16_t)0xF2ABU,
            .access_flags       = (uint8_t)DID_ACCESS_READ,
            .min_session        = (uint8_t)1U,   /* DEFAULT */
            .read_access_level  = (uint8_t)0U,
            .write_access_level = (uint8_t)0U,
            .data_length        = (uint16_t)2U,
            .read_cb            = t_read_f2ab,
            .write_cb           = NULL,
            .description        = "Test periodic DID",
        };
        (void)did_database_register(&s_test_did);
        g_db_ready = true;
    }

    (void)uds_periodic_init();
}

/* =========================================================================
 * Test suite
 * ========================================================================= */

ZTEST_SUITE(test_uds_periodic, NULL, NULL, NULL, NULL, NULL);

/* ---------------------------------------------------------------------- */

/**
 * TC-PERIODIC-001: uds_periodic_init() returns UDS_STATUS_OK.
 */
ZTEST(test_uds_periodic, tc001_init_ok)
{
    setup();
    uds_status_t rc = uds_periodic_init();
    zassert_equal(rc, UDS_STATUS_OK, "init must return OK");
}

/**
 * TC-PERIODIC-002: subscribe(0xAB, SLOW) returns OK; before any tick, nothing due.
 */
ZTEST(test_uds_periodic, tc002_subscribe_no_ticks_not_due)
{
    setup();
    uds_status_t rc = uds_periodic_subscribe((uint8_t)0xABU, UDS_PERIODIC_MODE_SLOW);
    zassert_equal(rc, UDS_STATUS_OK, "subscribe must return OK");

    uds_msg_buf_t frame;
    memset(&frame, 0, sizeof(frame));
    rc = uds_periodic_pop_due(&frame);
    zassert_equal(rc, UDS_STATUS_ERR_NOT_FOUND,
                  "no ticks yet — nothing should be due");
}

/**
 * TC-PERIODIC-003: re-subscribing the same ID updates in-place (slot count stays 1).
 * Verified by subscribing the same ID twice then filling 7 more unique IDs;
 * if the double-subscribe consumed 2 slots only 6 would be available and the
 * 7th would fail.
 */
ZTEST(test_uds_periodic, tc003_update_in_place)
{
    setup();
    zassert_equal(uds_periodic_subscribe((uint8_t)0x01U, UDS_PERIODIC_MODE_SLOW),
                  UDS_STATUS_OK, "first subscribe of 0x01 must succeed");
    zassert_equal(uds_periodic_subscribe((uint8_t)0x01U, UDS_PERIODIC_MODE_FAST),
                  UDS_STATUS_OK, "second subscribe of 0x01 (update) must succeed");

    /* Fill the 7 remaining slots with unique IDs 0x02..0x08 */
    uint8_t id;
    for (id = (uint8_t)0x02U; id <= (uint8_t)0x08U; id++) {
        uds_status_t rc = uds_periodic_subscribe(id, UDS_PERIODIC_MODE_MEDIUM);
        zassert_equal(rc, UDS_STATUS_OK, "slot fill must succeed (update kept count at 1)");
    }

    /* Table now full at 8 (0x01 counted once + 0x02..0x08 = 8 slots) */
    zassert_equal(uds_periodic_subscribe((uint8_t)0x09U, UDS_PERIODIC_MODE_SLOW),
                  UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  "9th unique ID must fail (table full at 8)");
}

/**
 * TC-PERIODIC-004: subscribe up to UDS_PERIODIC_MAX_SUBSCRIPTIONS → all OK.
 */
ZTEST(test_uds_periodic, tc004_fill_max_slots)
{
    setup();
    uint8_t i;
    for (i = (uint8_t)1U; i <= (uint8_t)UDS_PERIODIC_MAX_SUBSCRIPTIONS; i++) {
        uds_status_t rc = uds_periodic_subscribe(i, UDS_PERIODIC_MODE_SLOW);
        zassert_equal(rc, UDS_STATUS_OK, "subscribing up to max must succeed");
    }
}

/**
 * TC-PERIODIC-005: subscribe beyond max → ERR_REQUEST_OUT_OF_RANGE.
 */
ZTEST(test_uds_periodic, tc005_overflow_table)
{
    setup();
    uint8_t i;
    for (i = (uint8_t)1U; i <= (uint8_t)UDS_PERIODIC_MAX_SUBSCRIPTIONS; i++) {
        (void)uds_periodic_subscribe(i, UDS_PERIODIC_MODE_SLOW);
    }
    uds_status_t rc = uds_periodic_subscribe(
        (uint8_t)(UDS_PERIODIC_MAX_SUBSCRIPTIONS + 1U), UDS_PERIODIC_MODE_SLOW);
    zassert_equal(rc, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  "subscribe beyond max must return ERR_REQUEST_OUT_OF_RANGE");
}

/**
 * TC-PERIODIC-006: unsubscribe known ID frees the slot; a new ID can then be added.
 */
ZTEST(test_uds_periodic, tc006_unsubscribe_frees_slot)
{
    setup();
    uint8_t i;
    for (i = (uint8_t)1U; i <= (uint8_t)UDS_PERIODIC_MAX_SUBSCRIPTIONS; i++) {
        (void)uds_periodic_subscribe(i, UDS_PERIODIC_MODE_SLOW);
    }
    /* Table is full; unsubscribe slot 1 */
    uds_status_t rc = uds_periodic_unsubscribe((uint8_t)1U);
    zassert_equal(rc, UDS_STATUS_OK, "unsubscribe must return OK");

    /* Now there is exactly 1 free slot */
    rc = uds_periodic_subscribe((uint8_t)0xFFU, UDS_PERIODIC_MODE_SLOW);
    zassert_equal(rc, UDS_STATUS_OK, "subscribe after free must succeed");
}

/**
 * TC-PERIODIC-007: unsubscribe an ID not in the table → OK (no-op).
 */
ZTEST(test_uds_periodic, tc007_unsubscribe_unknown_noop)
{
    setup();
    uds_status_t rc = uds_periodic_unsubscribe((uint8_t)0xEEU);
    zassert_equal(rc, UDS_STATUS_OK, "unsubscribe unknown ID must be a no-op returning OK");
}

/**
 * TC-PERIODIC-008: cancel_all() removes all slots; pop_due returns ERR_NOT_FOUND.
 */
ZTEST(test_uds_periodic, tc008_cancel_all_clears)
{
    setup();
    (void)uds_periodic_subscribe((uint8_t)0xABU, UDS_PERIODIC_MODE_FAST);
    (void)uds_periodic_subscribe((uint8_t)0xCDU, UDS_PERIODIC_MODE_MEDIUM);

    uds_status_t rc = uds_periodic_cancel_all();
    zassert_equal(rc, UDS_STATUS_OK, "cancel_all must return OK");

    uds_msg_buf_t frame;
    memset(&frame, 0, sizeof(frame));
    rc = uds_periodic_pop_due(&frame);
    zassert_equal(rc, UDS_STATUS_ERR_NOT_FOUND,
                  "pop_due after cancel_all must return ERR_NOT_FOUND");
}

/**
 * TC-PERIODIC-009: tick_1ms() returns UDS_STATUS_OK on every call.
 */
ZTEST(test_uds_periodic, tc009_tick_always_ok)
{
    setup();
    uint16_t t;
    for (t = (uint16_t)0U; t < (uint16_t)20U; t++) {
        uds_status_t rc = uds_periodic_tick_1ms();
        zassert_equal(rc, UDS_STATUS_OK, "tick_1ms must always return OK");
    }
}

/**
 * TC-PERIODIC-010: FAST mode (10 ms): after exactly 10 ticks pop_due returns OK.
 * Requires test DID 0xF2AB to be registered (read_cb installed).
 */
ZTEST(test_uds_periodic, tc010_fast_fires_at_10ms)
{
    setup();
    (void)uds_periodic_subscribe((uint8_t)0xABU, UDS_PERIODIC_MODE_FAST);

    uint16_t t;
    for (t = (uint16_t)0U; t < (uint16_t)UDS_PERIODIC_FAST_MS; t++) {
        (void)uds_periodic_tick_1ms();
    }

    uds_msg_buf_t frame;
    memset(&frame, 0, sizeof(frame));
    uds_status_t rc = uds_periodic_pop_due(&frame);
    zassert_equal(rc, UDS_STATUS_OK,
                  "pop_due must return OK after FAST interval (10 ticks)");
}

/**
 * TC-PERIODIC-011: FAST mode (10 ms): after only 9 ticks nothing is due.
 */
ZTEST(test_uds_periodic, tc011_fast_not_due_before_interval)
{
    setup();
    (void)uds_periodic_subscribe((uint8_t)0xABU, UDS_PERIODIC_MODE_FAST);

    uint16_t t;
    for (t = (uint16_t)0U; t < (uint16_t)(UDS_PERIODIC_FAST_MS - (uint16_t)1U); t++) {
        (void)uds_periodic_tick_1ms();
    }

    uds_msg_buf_t frame;
    memset(&frame, 0, sizeof(frame));
    uds_status_t rc = uds_periodic_pop_due(&frame);
    zassert_equal(rc, UDS_STATUS_ERR_NOT_FOUND,
                  "pop_due must return ERR_NOT_FOUND before FAST interval expires");
}

/**
 * TC-PERIODIC-012: pop_due(NULL) → ERR_NULL_PTR.
 */
ZTEST(test_uds_periodic, tc012_pop_due_null_ptr)
{
    setup();
    uds_status_t rc = uds_periodic_pop_due(NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR,
                  "NULL out_frame must return ERR_NULL_PTR");
}

/**
 * TC-PERIODIC-013: pop_due with no subscriptions → ERR_NOT_FOUND.
 */
ZTEST(test_uds_periodic, tc013_pop_due_empty_table)
{
    setup();
    uds_msg_buf_t frame;
    memset(&frame, 0, sizeof(frame));
    uds_status_t rc = uds_periodic_pop_due(&frame);
    zassert_equal(rc, UDS_STATUS_ERR_NOT_FOUND,
                  "pop_due on empty table must return ERR_NOT_FOUND");
}

/**
 * TC-PERIODIC-014: frame format after FAST fires: data[0]=0x6A, data[1]=periodicId,
 *                  length == 2 + data_length (2 + 2 = 4 for test DID).
 */
ZTEST(test_uds_periodic, tc014_frame_format)
{
    setup();
    (void)uds_periodic_subscribe((uint8_t)0xABU, UDS_PERIODIC_MODE_FAST);

    uint16_t t;
    for (t = (uint16_t)0U; t < (uint16_t)UDS_PERIODIC_FAST_MS; t++) {
        (void)uds_periodic_tick_1ms();
    }

    uds_msg_buf_t frame;
    memset(&frame, 0, sizeof(frame));
    (void)uds_periodic_pop_due(&frame);

    zassert_equal(frame.data[0], (uint8_t)0x6AU,
                  "frame[0] must be 0x6A (positive response for SID 0x2A)");
    zassert_equal(frame.data[1], (uint8_t)0xABU,
                  "frame[1] must be the periodicDataIdentifier (0xAB)");
    zassert_equal(frame.length, (uint16_t)4U,
                  "frame length must be 2 (header) + 2 (data) = 4");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_uds_periodic__tc001_init_ok(void);
extern void test_uds_periodic__tc002_subscribe_no_ticks_not_due(void);
extern void test_uds_periodic__tc003_update_in_place(void);
extern void test_uds_periodic__tc004_fill_max_slots(void);
extern void test_uds_periodic__tc005_overflow_table(void);
extern void test_uds_periodic__tc006_unsubscribe_frees_slot(void);
extern void test_uds_periodic__tc007_unsubscribe_unknown_noop(void);
extern void test_uds_periodic__tc008_cancel_all_clears(void);
extern void test_uds_periodic__tc009_tick_always_ok(void);
extern void test_uds_periodic__tc010_fast_fires_at_10ms(void);
extern void test_uds_periodic__tc011_fast_not_due_before_interval(void);
extern void test_uds_periodic__tc012_pop_due_null_ptr(void);
extern void test_uds_periodic__tc013_pop_due_empty_table(void);
extern void test_uds_periodic__tc014_frame_format(void);

void run_all_tests(void)
{
    RUN_TEST(test_uds_periodic__tc001_init_ok);
    RUN_TEST(test_uds_periodic__tc002_subscribe_no_ticks_not_due);
    RUN_TEST(test_uds_periodic__tc003_update_in_place);
    RUN_TEST(test_uds_periodic__tc004_fill_max_slots);
    RUN_TEST(test_uds_periodic__tc005_overflow_table);
    RUN_TEST(test_uds_periodic__tc006_unsubscribe_frees_slot);
    RUN_TEST(test_uds_periodic__tc007_unsubscribe_unknown_noop);
    RUN_TEST(test_uds_periodic__tc008_cancel_all_clears);
    RUN_TEST(test_uds_periodic__tc009_tick_always_ok);
    RUN_TEST(test_uds_periodic__tc010_fast_fires_at_10ms);
    RUN_TEST(test_uds_periodic__tc011_fast_not_due_before_interval);
    RUN_TEST(test_uds_periodic__tc012_pop_due_null_ptr);
    RUN_TEST(test_uds_periodic__tc013_pop_due_empty_table);
    RUN_TEST(test_uds_periodic__tc014_frame_format);
}
