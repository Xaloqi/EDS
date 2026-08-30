// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_dtc_mirror.c
 *
 * MODULE UNDER TEST: config/dtc_mirror.c
 *
 * PURPOSE:
 *   Focused unit test for the DTC NVM mirror module.
 *
 *   This file directly validates the H3 fix path — specifically that
 *   dtc_mirror_init() and dtc_mirror_load() exist, behave correctly, and
 *   exhibit the soft-degrade behaviour required by the generated uds_init.c
 *   (Steps 3.5 and 5.5 added by the P0-1 fix).
 *
 *   The complementary test_phase3_nvm_dtc.c covers high-level round-trip
 *   scenarios (power-cycle survival).  This file isolates the module's own
 *   API contract at the unit level:
 *
 *   dtc_mirror_init():
 *     TC-MIRROR-001  First call → UDS_STATUS_OK
 *     TC-MIRROR-002  dtc_mirror_is_ready() returns true after init
 *     TC-MIRROR-003  Second call without reset → ERR_ALREADY_INITIALIZED
 *     TC-MIRROR-004  dtc_mirror_is_ready() returns false before init
 *
 *   dtc_mirror_load() — NVM not ready (soft-degrade, H3 fix path):
 *     TC-MIRROR-005  load() before init → ERR_NOT_INITIALIZED
 *     TC-MIRROR-006  load() after init but NVM not ready → ERR_NOT_INITIALIZED
 *     TC-MIRROR-007  load() on first boot (NVM ready, no record) → OK, status unchanged
 *     TC-MIRROR-008  load() restores persisted status bytes correctly
 *     TC-MIRROR-009  load() skips DTC codes absent from current database
 *     TC-MIRROR-010  load() after NVM ready is idempotent (second call same result)
 *
 *   dtc_mirror_flush_all():
 *     TC-MIRROR-011  flush_all() before init → ERR_NOT_INITIALIZED
 *     TC-MIRROR-012  flush_all() with NVM not ready → ERR_NOT_INITIALIZED
 *     TC-MIRROR-013  flush_all() writes valid NVM record (header count correct)
 *     TC-MIRROR-014  flush_all() with empty DTC table writes count=0 header
 *
 *   dtc_mirror_save_one():
 *     TC-MIRROR-015  save_one() before init → ERR_NOT_INITIALIZED
 *     TC-MIRROR-016  save_one() triggers full flush (NVM record updated)
 *     TC-MIRROR-017  save_one() after clear: new status visible in NVM
 *
 *   dtc_mirror_clear_all():
 *     TC-MIRROR-018  clear_all() before init → ERR_NOT_INITIALIZED
 *     TC-MIRROR-019  clear_all() writes all-zero status bytes to NVM
 *     TC-MIRROR-020  clear_all() + load() restores status bytes as zero
 *
 *   H3 fix integration:
 *     TC-MIRROR-021  Correct call order: init → register → load → status OK
 *     TC-MIRROR-022  Soft-degrade: load ERR_NOT_INITIALIZED is non-fatal in
 *                    the uds_generated_init() pattern (does not block init chain)
 *     TC-MIRROR-023  Status bytes survive: set → flush → power-cycle → load
 *
 *   Integrity checking (issue #114 — DTC mirror header/CRC fix):
 *     TC-MIRROR-024  Bit-flip in a flushed entry → load() reports
 *                    ERR_NVM_DATA_CORRUPT and applies nothing (not partial)
 *     TC-MIRROR-025  Record truncated below what its own header declares →
 *                    load() reports ERR_NVM_DATA_CORRUPT (was the original
 *                    bug: silently applied whatever fit and returned OK)
 *     TC-MIRROR-026  Legacy pre-header record (no magic) → load() returns
 *                    OK and discards it (deliberate migration decision)
 *     TC-MIRROR-027  Valid magic + unsupported version → load() reports
 *                    ERR_NVM_DATA_CORRUPT
 *
 *   Mirror sizing vs. the NVM record cap (issue #123):
 *     TC-MIRROR-028  DTC_MIRROR_MAX_BYTES (today's persisted-DTC cap) fits
 *                    within NVM_MAX_RECORD_BYTES — the fix is internally
 *                    consistent (the _Static_assert in dtc_mirror.c is the
 *                    primary enforcement; this is a runtime cross-check).
 *     TC-MIRROR-029  Proves issue #123 was a real bug, independent of
 *                    TC-MIRROR-028: recomputes the OLD (pre-fix) sizing
 *                    formula — header + UDS_MAX_DTC_COUNT entries + CRC —
 *                    directly from the raw wire-format constants, and
 *                    shows it exceeds NVM_MAX_RECORD_BYTES.
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h for host compilation)
 * NVM:       NVM_STORE_HOST_MOCK (RAM-backed mock, controlled via nvm_mock_reset /
 *            nvm_mock_deinit)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "dtc_mirror.h"
#include "dtc_database.h"
#include "nvm_store.h"
#include "uds_types.h"

/* ==========================================================================
 * External test-only hooks (guarded by UNIT_TEST / NVM_STORE_HOST_MOCK in
 * their respective translation units)
 * ========================================================================== */

extern void nvm_mock_reset(void);
extern void nvm_mock_deinit(void);
extern void dtc_database_test_reset(void);
extern void dtc_mirror_test_reset(void);

/* ==========================================================================
 * Test DTC codes
 * ========================================================================== */

#define DTC_1  (0xC00100UL)
#define DTC_2  (0xC00200UL)
#define DTC_3  (0xC00300UL)

/* ==========================================================================
 * setUp / tearDown
 *
 * Each test starts with:
 *   - NVM mock reset (blank, uninitialized flash)
 *   - NVM mock initialized (simulates driver mount at ECU boot)
 *   - DTC database reset and initialized (empty, ready for registration)
 *   - DTC mirror reset (clears s_initialized flag)
 *   - DTC mirror NOT initialized yet (each test controls its own init)
 *
 * This matches the order mandated by dtc_mirror.h:
 *   nvm_store_init() → dtc_database_init() → dtc_mirror_init() → register
 *   → dtc_mirror_load()
 * ========================================================================== */

void setUp(void)
{
    nvm_mock_reset();
    (void)nvm_store_init(NULL);
    dtc_database_test_reset();
    (void)dtc_database_init();
    dtc_mirror_test_reset();
    /* dtc_mirror_init() deliberately NOT called here — each test calls it
     * when needed so that pre-init guard tests work correctly. */
}

void tearDown(void) {}

/* ==========================================================================
 * Helper: re-initialize everything as if it's a fresh power-cycle that
 * preserves NVM data (nvm_mock_deinit, not nvm_mock_reset).
 * Used by tests that verify persistence across simulated resets.
 * ========================================================================== */

static void simulate_power_cycle(void)
{
    nvm_mock_deinit();           /* clear initialized flag, keep data */
    (void)nvm_store_init(NULL);  /* remount — data still present */
    dtc_database_test_reset();
    (void)dtc_database_init();
    dtc_mirror_test_reset();
    (void)dtc_mirror_init();
}

/* ==========================================================================
 * Suite declaration
 * ========================================================================== */

ZTEST_SUITE(dtc_mirror, NULL, NULL, NULL, NULL, NULL);

/* ==========================================================================
 * dtc_mirror_init() tests
 * ========================================================================== */

/* TC-MIRROR-001  First call → UDS_STATUS_OK */
ZTEST(dtc_mirror, test_init_first_call_ok)
{
    zassert_equal(UDS_STATUS_OK, dtc_mirror_init(), "first init must return OK");
}

/* TC-MIRROR-002  dtc_mirror_is_ready() returns true after init */
ZTEST(dtc_mirror, test_is_ready_true_after_init)
{
    (void)dtc_mirror_init();
    zassert_true(dtc_mirror_is_ready(), "is_ready must be true after init");
}

/* TC-MIRROR-003  Second call without reset → ERR_ALREADY_INITIALIZED */
ZTEST(dtc_mirror, test_init_second_call_returns_already_initialized)
{
    (void)dtc_mirror_init();
    zassert_equal(UDS_STATUS_ERR_ALREADY_INITIALIZED,
                  dtc_mirror_init(),
                  "second init must return ALREADY_INITIALIZED");
}

/* TC-MIRROR-004  dtc_mirror_is_ready() returns false before init */
ZTEST(dtc_mirror, test_is_ready_false_before_init)
{
    /* setUp left the mirror in reset state — not initialized. */
    zassert_false(dtc_mirror_is_ready(), "is_ready must be false before init");
}

/* ==========================================================================
 * dtc_mirror_load() tests
 * ========================================================================== */

/* TC-MIRROR-005  load() before init → ERR_NOT_INITIALIZED */
ZTEST(dtc_mirror, test_load_before_init_not_initialized)
{
    /* Mirror not initialized (setUp did not call dtc_mirror_init). */
    zassert_equal(UDS_STATUS_ERR_NOT_INITIALIZED,
                  dtc_mirror_load(),
                  "load before init must return NOT_INITIALIZED");
}

/* TC-MIRROR-006  load() after init but NVM not ready → ERR_NOT_INITIALIZED
 *
 * This is the H3 soft-degrade path. The generated uds_init.c Step 5.5 treats
 * ERR_NOT_INITIALIZED from dtc_mirror_load() as non-fatal. This test confirms:
 *   (a) the return code is exactly ERR_NOT_INITIALIZED (not some other error)
 *   (b) the condition is reproducible when nvm_store is not yet initialized
 */
ZTEST(dtc_mirror, test_load_nvm_not_ready_soft_degrade)
{
    (void)dtc_mirror_init();

    /* Simulate NVM driver not yet mounted (teardown the mock). */
    nvm_mock_deinit();   /* clears s_initialized — nvm_store_is_ready() = false */

    uds_status_t rc = dtc_mirror_load();

    /* Must return NOT_INITIALIZED, not OK or PLATFORM. */
    zassert_equal(UDS_STATUS_ERR_NOT_INITIALIZED, rc,
                  "load with NVM not ready must return NOT_INITIALIZED (H3 soft-degrade)");
}

/* TC-MIRROR-007  load() on first boot (NVM ready, no record) → OK, status unchanged */
ZTEST(dtc_mirror, test_load_first_boot_no_nvm_record)
{
    (void)dtc_mirror_init();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_register(DTC_2, 0x00U, "DTC_2");

    /* No NVM record written yet — first boot. */
    zassert_equal(UDS_STATUS_OK, dtc_mirror_load(),
                  "first-boot load (no record) must return OK");

    /* Status bytes must remain at their initial value. */
    dtc_entry_t *e1 = dtc_database_find(DTC_1);
    dtc_entry_t *e2 = dtc_database_find(DTC_2);
    zassert_not_null(e1, "DTC_1 must exist");
    zassert_not_null(e2, "DTC_2 must exist");
    zassert_equal(0x00U, e1->status_byte, "DTC_1 status unchanged on first boot");
    zassert_equal(0x00U, e2->status_byte, "DTC_2 status unchanged on first boot");
}

/* TC-MIRROR-008  load() restores persisted status bytes correctly */
ZTEST(dtc_mirror, test_load_restores_status_bytes)
{
    (void)dtc_mirror_init();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_register(DTC_2, 0x00U, "DTC_2");
    (void)dtc_database_register(DTC_3, 0x00U, "DTC_3");

    /* Set known non-zero status bytes and flush. */
    (void)dtc_database_set_status(DTC_1, 0x09U);  /* CONFIRMED | TEST_FAILED */
    (void)dtc_database_set_status(DTC_2, 0x04U);  /* PENDING */
    (void)dtc_database_set_status(DTC_3, 0x00U);  /* cleared */
    (void)dtc_mirror_flush_all();

    /* Simulate power-cycle with data preserved. */
    simulate_power_cycle();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_register(DTC_2, 0x00U, "DTC_2");
    (void)dtc_database_register(DTC_3, 0x00U, "DTC_3");

    zassert_equal(UDS_STATUS_OK, dtc_mirror_load(), "load after power-cycle must return OK");

    zassert_equal(0x09U, dtc_database_find(DTC_1)->status_byte, "DTC_1 must be 0x09");
    zassert_equal(0x04U, dtc_database_find(DTC_2)->status_byte, "DTC_2 must be 0x04");
    zassert_equal(0x00U, dtc_database_find(DTC_3)->status_byte, "DTC_3 must be 0x00");
}

/* TC-MIRROR-009  load() skips DTC codes absent from current database */
ZTEST(dtc_mirror, test_load_skips_unregistered_codes)
{
    (void)dtc_mirror_init();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_register(DTC_2, 0x00U, "DTC_2");
    (void)dtc_database_register(DTC_3, 0x00U, "DTC_3");

    (void)dtc_database_set_status(DTC_1, 0x09U);
    (void)dtc_database_set_status(DTC_2, 0x04U);
    (void)dtc_database_set_status(DTC_3, 0x08U);
    (void)dtc_mirror_flush_all();

    /* Power-cycle: firmware update removed DTC_3. */
    simulate_power_cycle();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_register(DTC_2, 0x00U, "DTC_2");
    /* DTC_3 intentionally NOT registered. */

    /* Load must succeed without crash or error. */
    zassert_equal(UDS_STATUS_OK, dtc_mirror_load(),
                  "load with removed DTC in NVM must return OK");

    /* DTC_1 and DTC_2 restored; DTC_3 is simply absent. */
    zassert_equal(0x09U, dtc_database_find(DTC_1)->status_byte, "DTC_1 restored");
    zassert_equal(0x04U, dtc_database_find(DTC_2)->status_byte, "DTC_2 restored");
    zassert_is_null(dtc_database_find(DTC_3), "DTC_3 must not exist in new database");
}

/* TC-MIRROR-010  load() is idempotent — calling it twice produces same result */
ZTEST(dtc_mirror, test_load_idempotent)
{
    (void)dtc_mirror_init();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_set_status(DTC_1, 0x09U);
    (void)dtc_mirror_flush_all();

    simulate_power_cycle();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");

    zassert_equal(UDS_STATUS_OK, dtc_mirror_load(), "first load OK");
    zassert_equal(0x09U, dtc_database_find(DTC_1)->status_byte, "after first load");

    /* Second load — status already correct, must still return OK. */
    zassert_equal(UDS_STATUS_OK, dtc_mirror_load(), "second load must return OK");
    zassert_equal(0x09U, dtc_database_find(DTC_1)->status_byte, "after second load");
}

/* ==========================================================================
 * dtc_mirror_flush_all() tests
 * ========================================================================== */

/* TC-MIRROR-011  flush_all() before init → ERR_NOT_INITIALIZED */
ZTEST(dtc_mirror, test_flush_all_before_init)
{
    zassert_equal(UDS_STATUS_ERR_NOT_INITIALIZED,
                  dtc_mirror_flush_all(),
                  "flush_all before init must return NOT_INITIALIZED");
}

/* TC-MIRROR-012  flush_all() with NVM not ready → ERR_NOT_INITIALIZED */
ZTEST(dtc_mirror, test_flush_all_nvm_not_ready)
{
    (void)dtc_mirror_init();
    nvm_mock_deinit();   /* make NVM unavailable */

    zassert_equal(UDS_STATUS_ERR_NOT_INITIALIZED,
                  dtc_mirror_flush_all(),
                  "flush_all with NVM not ready must return NOT_INITIALIZED");
}

/* TC-MIRROR-013  flush_all() writes valid NVM record (header count correct) */
ZTEST(dtc_mirror, test_flush_all_writes_correct_header)
{
    (void)dtc_mirror_init();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_register(DTC_2, 0x00U, "DTC_2");
    (void)dtc_database_set_status(DTC_1, 0x09U);
    (void)dtc_database_set_status(DTC_2, 0x04U);

    zassert_equal(UDS_STATUS_OK, dtc_mirror_flush_all(), "flush_all must return OK");

    /* Verify NVM record exists and has the v1 integrity-checked layout:
     * [magic:2][version:1][count:2][entry×2][crc32:4] = 5 + 8 + 4 = 17 bytes. */
    uint8_t buf[64];
    size_t  len = 0U;
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_read(NVM_KEY_DTC_MIRROR, buf, sizeof(buf), &len),
                  "NVM record must exist after flush");

    zassert_equal((size_t)17U, len,
                  "record length must be header(5) + 2 entries(8) + crc(4) = 17");
    zassert_equal(DTC_MIRROR_MAGIC_0, buf[0], "magic byte 0 must match");
    zassert_equal(DTC_MIRROR_MAGIC_1, buf[1], "magic byte 1 must match");
    zassert_equal((uint8_t)DTC_MIRROR_FORMAT_VERSION, buf[2], "version must match");

    /* Count bytes: big-endian.  2 DTCs registered → count == 2. */
    uint16_t count = (uint16_t)(((uint16_t)buf[3] << 8U) | (uint16_t)buf[4]);
    zassert_equal(2U, count, "NVM mirror count must be 2");
}

/* TC-MIRROR-014  flush_all() with empty DTC table writes count=0 header */
ZTEST(dtc_mirror, test_flush_all_empty_table_writes_zero_count)
{
    (void)dtc_mirror_init();
    /* No DTCs registered. */

    zassert_equal(UDS_STATUS_OK, dtc_mirror_flush_all(),
                  "flush_all on empty table must return OK");

    uint8_t buf[16];
    size_t  len = 0U;
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_read(NVM_KEY_DTC_MIRROR, buf, sizeof(buf), &len),
                  "NVM record must exist");

    /* Header + crc only, no entries: 5 + 0 + 4 = 9 bytes, count = 0. */
    zassert_equal((size_t)9U, len, "empty flush must produce a 9-byte record");
    zassert_equal(DTC_MIRROR_MAGIC_0, buf[0], "magic byte 0 must match");
    zassert_equal(DTC_MIRROR_MAGIC_1, buf[1], "magic byte 1 must match");
    zassert_equal((uint8_t)DTC_MIRROR_FORMAT_VERSION, buf[2], "version must match");
    zassert_equal(0x00U, buf[3], "count high byte must be 0");
    zassert_equal(0x00U, buf[4], "count low byte must be 0");
}

/* ==========================================================================
 * dtc_mirror_save_one() tests
 * ========================================================================== */

/* TC-MIRROR-015  save_one() before init → ERR_NOT_INITIALIZED */
ZTEST(dtc_mirror, test_save_one_before_init)
{
    zassert_equal(UDS_STATUS_ERR_NOT_INITIALIZED,
                  dtc_mirror_save_one(DTC_1, 0x09U),
                  "save_one before init must return NOT_INITIALIZED");
}

/* TC-MIRROR-016  save_one() triggers a full flush and NVM record is updated */
ZTEST(dtc_mirror, test_save_one_triggers_flush)
{
    (void)dtc_mirror_init();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_set_status(DTC_1, 0x0FU);

    zassert_equal(UDS_STATUS_OK, dtc_mirror_save_one(DTC_1, 0x0FU),
                  "save_one must return OK");

    /* NVM must now contain the record. */
    uint8_t buf[32];
    size_t  len = 0U;
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_read(NVM_KEY_DTC_MIRROR, buf, sizeof(buf), &len),
                  "NVM record must exist after save_one");
    zassert_true(len >= 6U, "NVM record too short after save_one");
}

/* TC-MIRROR-017  save_one() after clear: new status visible in NVM after reload */
ZTEST(dtc_mirror, test_save_one_after_clear_persists)
{
    (void)dtc_mirror_init();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_set_status(DTC_1, 0xFFU);
    (void)dtc_mirror_flush_all();

    /* Clear status and persist via save_one. */
    (void)dtc_database_set_status(DTC_1, 0x00U);
    zassert_equal(UDS_STATUS_OK, dtc_mirror_save_one(DTC_1, 0x00U),
                  "save_one with cleared status must return OK");

    /* Power-cycle and reload. */
    simulate_power_cycle();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_set_status(DTC_1, 0xFFU);  /* set dirty to confirm load overwrites */
    zassert_equal(UDS_STATUS_OK, dtc_mirror_load(), "reload must return OK");

    /* Status must be 0x00 (what was saved, not the pre-load 0xFF). */
    zassert_equal(0x00U, dtc_database_find(DTC_1)->status_byte,
                  "save_one 0x00 must persist across power-cycle");
}

/* ==========================================================================
 * dtc_mirror_clear_all() tests
 * ========================================================================== */

/* TC-MIRROR-018  clear_all() before init → ERR_NOT_INITIALIZED */
ZTEST(dtc_mirror, test_clear_all_before_init)
{
    zassert_equal(UDS_STATUS_ERR_NOT_INITIALIZED,
                  dtc_mirror_clear_all(),
                  "clear_all before init must return NOT_INITIALIZED");
}

/* TC-MIRROR-019  clear_all() writes all-zero status bytes to NVM */
ZTEST(dtc_mirror, test_clear_all_writes_zeros_to_nvm)
{
    (void)dtc_mirror_init();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_register(DTC_2, 0x00U, "DTC_2");
    (void)dtc_database_set_status(DTC_1, 0x09U);
    (void)dtc_database_set_status(DTC_2, 0x04U);
    (void)dtc_mirror_flush_all();   /* write non-zero statuses first */

    zassert_equal(UDS_STATUS_OK, dtc_mirror_clear_all(),
                  "clear_all must return OK");

    /* After clear_all + power-cycle, reloaded status bytes must all be 0x00. */
    simulate_power_cycle();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_register(DTC_2, 0x00U, "DTC_2");
    /* Pre-set dirty to confirm load overwrites. */
    (void)dtc_database_set_status(DTC_1, 0xFFU);
    (void)dtc_database_set_status(DTC_2, 0xFFU);

    zassert_equal(UDS_STATUS_OK, dtc_mirror_load(), "load after clear_all must return OK");

    zassert_equal(0x00U, dtc_database_find(DTC_1)->status_byte,
                  "DTC_1 status must be 0x00 after clear_all");
    zassert_equal(0x00U, dtc_database_find(DTC_2)->status_byte,
                  "DTC_2 status must be 0x00 after clear_all");
}

/* TC-MIRROR-020  clear_all() + load() in the same session: reloaded status is 0 */
ZTEST(dtc_mirror, test_clear_all_then_load_in_same_session)
{
    (void)dtc_mirror_init();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_set_status(DTC_1, 0x09U);
    (void)dtc_mirror_flush_all();

    /* Clear then immediately load in the same session. */
    (void)dtc_mirror_clear_all();

    simulate_power_cycle();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_set_status(DTC_1, 0xFFU);  /* dirty */

    zassert_equal(UDS_STATUS_OK, dtc_mirror_load(), "");
    zassert_equal(0x00U, dtc_database_find(DTC_1)->status_byte,
                  "clear_all must win over earlier flush");
}

/* ==========================================================================
 * H3 fix integration tests
 * ========================================================================== */

/*
 * TC-MIRROR-021  Correct call order: init → register → load → status OK
 *
 * This is the canonical sequence produced by the fixed uds_init.c template
 * (Steps 3.5 + 5.5 from the P0-1 fix).  Validates that the mandated order
 * works end-to-end with a non-trivial payload.
 */
ZTEST(dtc_mirror, test_h3_fix_correct_call_order)
{
    /* Exactly mirrors Steps 3/3.5/4/5/5.5 from generated/uds_init.c. */

    /* Step 3 already done in setUp: dtc_database_init() */

    /* Step 3.5: dtc_mirror_init() */
    zassert_equal(UDS_STATUS_OK, dtc_mirror_init(),
                  "Step 3.5: dtc_mirror_init must return OK");

    /* Step 4/5: register DTCs (simulates DTC registration loop). */
    (void)dtc_database_register(DTC_1, 0x00U, "CAN_loss");
    (void)dtc_database_register(DTC_2, 0x00U, "Transport_timeout");

    /* Pre-populate NVM to simulate previous session (non-zero statuses). */
    (void)dtc_database_set_status(DTC_1, 0x09U);
    (void)dtc_database_set_status(DTC_2, 0x04U);
    (void)dtc_mirror_flush_all();

    /* Power-cycle: keep NVM data. */
    nvm_mock_deinit();
    (void)nvm_store_init(NULL);
    dtc_database_test_reset();
    (void)dtc_database_init();
    dtc_mirror_test_reset();

    /* Replay Steps 3.5, 5, 5.5. */
    zassert_equal(UDS_STATUS_OK, dtc_mirror_init(),
                  "Step 3.5 after power-cycle must return OK");

    (void)dtc_database_register(DTC_1, 0x00U, "CAN_loss");
    (void)dtc_database_register(DTC_2, 0x00U, "Transport_timeout");

    /* Step 5.5: dtc_mirror_load() */
    zassert_equal(UDS_STATUS_OK, dtc_mirror_load(),
                  "Step 5.5: dtc_mirror_load must return OK");

    /* Status bytes must be restored from NVM. */
    zassert_equal(0x09U, dtc_database_find(DTC_1)->status_byte,
                  "H3 fix: DTC_1 status must be restored by load");
    zassert_equal(0x04U, dtc_database_find(DTC_2)->status_byte,
                  "H3 fix: DTC_2 status must be restored by load");
}

/*
 * TC-MIRROR-022  Soft-degrade: ERR_NOT_INITIALIZED from load() is non-fatal
 *
 * The generated uds_init.c Step 5.5 soft-degrade logic (from the P0-1 fix):
 *
 *   uds_status_t mirror_rc = dtc_mirror_load();
 *   if ((mirror_rc != UDS_STATUS_OK) &&
 *       (mirror_rc != UDS_STATUS_ERR_NOT_INITIALIZED) &&
 *       (mirror_rc != UDS_STATUS_ERR_PLATFORM)) {
 *       return mirror_rc;  // only propagate unexpected errors
 *   }
 *   // ERR_NOT_INITIALIZED and ERR_PLATFORM are non-fatal
 *
 * This test validates that the exact error code produced when NVM is not
 * ready matches the codes the generated init pattern will absorb.
 */
ZTEST(dtc_mirror, test_h3_fix_soft_degrade_nvm_not_ready)
{
    (void)dtc_mirror_init();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");

    /* NVM not ready — simulates NVM driver not yet mounted at boot. */
    nvm_mock_deinit();

    uds_status_t rc = dtc_mirror_load();

    /* Verify the returned code is one that uds_generated_init() absorbs. */
    bool is_soft_degrade = (rc == UDS_STATUS_ERR_NOT_INITIALIZED) ||
                           (rc == UDS_STATUS_ERR_PLATFORM);

    zassert_true(is_soft_degrade,
                 "H3 soft-degrade: load with NVM unavailable must return "
                 "ERR_NOT_INITIALIZED or ERR_PLATFORM (not a fatal error)");

    /* The DTC table must still be intact — no side effects from failed load. */
    zassert_not_null(dtc_database_find(DTC_1),
                     "DTC_1 must still be registered after failed load");
}

/*
 * TC-MIRROR-023  Full persistence scenario: set → flush → power-cycle → load
 *
 * End-to-end proof that the H3 fix closes the fault-history loss bug.
 * Before the fix, DTC status was reset to 0x00 on every ECU reset because
 * dtc_mirror_init/load were never called.  After the fix, this test must pass.
 */
ZTEST(dtc_mirror, test_h3_fix_full_persistence_scenario)
{
    /* --- Boot #1: fault occurs, status set and persisted --- */
    (void)dtc_mirror_init();
    (void)dtc_database_register(DTC_1, 0x00U, "CAN_loss");
    (void)dtc_database_register(DTC_2, 0x00U, "Transport_timeout");

    /* Faults detected by application. */
    (void)dtc_database_set_status(DTC_1, 0x09U);   /* CONFIRMED | TEST_FAILED */
    (void)dtc_database_set_status(DTC_2, 0x00U);   /* no fault */

    /* dtc_mirror_flush_all() called before ECU reset (e.g. from 0x11 handler). */
    zassert_equal(UDS_STATUS_OK, dtc_mirror_flush_all(), "flush before reset must succeed");

    /* --- Power-cycle --- */
    simulate_power_cycle();  /* NVM data preserved */

    /* --- Boot #2: re-register DTCs, load mirror --- */
    (void)dtc_database_register(DTC_1, 0x00U, "CAN_loss");
    (void)dtc_database_register(DTC_2, 0x00U, "Transport_timeout");

    /* Without the H3 fix: dtc_mirror_load() would never be called here.
     * With the fix: it IS called (Step 5.5 in generated/uds_init.c).
     * This test calls it directly to prove the mechanism works. */
    zassert_equal(UDS_STATUS_OK, dtc_mirror_load(),
                  "H3 fix: dtc_mirror_load must succeed on boot #2");

    /* Fault history must survive the reset. */
    zassert_equal(0x09U, dtc_database_find(DTC_1)->status_byte,
                  "H3 fix: DTC_1 fault history must survive power-cycle");
    zassert_equal(0x00U, dtc_database_find(DTC_2)->status_byte,
                  "H3 fix: DTC_2 cleared status must survive power-cycle");
}

/* ==========================================================================
 * Integrity checking tests (issue #114)
 *
 * These craft raw NVM bytes directly (bypassing dtc_mirror's own writer) to
 * simulate flash corruption / a truncated write / a pre-fix legacy record,
 * then assert dtc_mirror_load()'s response distinguishes them per the
 * DELIBERATE cases documented in dtc_mirror.h:
 *   (1) no/legacy record → OK, nothing applied
 *   (2) valid record     → OK, restored (covered by TC-MIRROR-008 etc.)
 *   (3) corrupt record   → ERR_NVM_DATA_CORRUPT, nothing applied
 * ========================================================================== */

/* TC-MIRROR-024  Bit-flip in a flushed entry (CRC no longer matches) →
 * load() reports ERR_NVM_DATA_CORRUPT and applies NOTHING (all-or-nothing,
 * not a partial apply). */
ZTEST(dtc_mirror, test_load_reports_corrupt_on_bad_crc)
{
    uint8_t raw[32];
    size_t  raw_len = 0U;

    (void)dtc_mirror_init();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_set_status(DTC_1, 0x09U);
    zassert_equal(UDS_STATUS_OK, dtc_mirror_flush_all(), "flush must succeed");

    zassert_equal(UDS_STATUS_OK,
                  nvm_store_read(NVM_KEY_DTC_MIRROR, raw, sizeof(raw), &raw_len),
                  "must read back the flushed record");
    zassert_equal((size_t)13U, raw_len, "1-entry record must be 5+4+4=13 bytes");

    /* Flip the entry's status byte (offset 5+3=8) without touching the CRC
     * trailer — simulates a flash bit-flip after a valid write. */
    raw[8] ^= 0xFFU;
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_write(NVM_KEY_DTC_MIRROR, raw, raw_len),
                  "must be able to write back the corrupted record");

    /* Power-cycle: fresh database, DTC_1 defaults back to 0x00. */
    simulate_power_cycle();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");

    uds_status_t rc = dtc_mirror_load();

    zassert_equal(UDS_STATUS_ERR_NVM_DATA_CORRUPT, rc,
                  "CRC-mismatched record must be reported as a distinct corrupt condition");
    zassert_equal(0x00U, dtc_database_find(DTC_1)->status_byte,
                  "corrupt record must not be applied at all, not even partially");
}

/* TC-MIRROR-025  Record truncated below what its own header declares →
 * load() reports ERR_NVM_DATA_CORRUPT. This is the exact original bug
 * shape: a short read mid-loop used to `break` and still return OK with
 * whatever entries had been parsed so far. */
ZTEST(dtc_mirror, test_load_reports_corrupt_on_truncated_record)
{
    uint8_t raw[32];
    size_t  raw_len = 0U;

    (void)dtc_mirror_init();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_register(DTC_2, 0x00U, "DTC_2");
    (void)dtc_database_set_status(DTC_1, 0x09U);
    (void)dtc_database_set_status(DTC_2, 0x04U);
    zassert_equal(UDS_STATUS_OK, dtc_mirror_flush_all(), "flush must succeed");

    zassert_equal(UDS_STATUS_OK,
                  nvm_store_read(NVM_KEY_DTC_MIRROR, raw, sizeof(raw), &raw_len),
                  "must read back the flushed record");
    zassert_equal((size_t)17U, raw_len, "2-entry record must be 5+8+4=17 bytes");

    /* Header still declares count=2 (bytes[3..5) unchanged), but write back
     * only header + first entry — second entry and CRC trailer missing. */
    size_t truncated_len = (size_t)9U; /* 5-byte header + 1 entry, no CRC */
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_write(NVM_KEY_DTC_MIRROR, raw, truncated_len),
                  "must be able to write back the truncated record");

    simulate_power_cycle();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");
    (void)dtc_database_register(DTC_2, 0x00U, "DTC_2");

    uds_status_t rc = dtc_mirror_load();

    zassert_equal(UDS_STATUS_ERR_NVM_DATA_CORRUPT, rc,
                  "truncated record must be reported as corrupt, not silently accepted");
    zassert_equal(0x00U, dtc_database_find(DTC_1)->status_byte,
                  "truncated record must not partially apply DTC_1 (issue #114)");
    zassert_equal(0x00U, dtc_database_find(DTC_2)->status_byte,
                  "truncated record must not partially apply DTC_2 (issue #114)");
}

/* TC-MIRROR-026  A legacy pre-header record (the old [count:2][entries...]
 * format, no magic, as left on flash by firmware built before issue #114)
 * is discarded like "no mirror yet" — OK, nothing applied. This is the
 * deliberate, documented migration decision (see dtc_mirror.h). */
ZTEST(dtc_mirror, test_load_treats_legacy_record_as_no_mirror)
{
    uint8_t legacy[6];
    size_t  pos = 0U;

    /* Old wire format: [count_hi][count_lo][code_b2][code_b1][code_b0][status] */
    legacy[pos++] = 0x00U;                                  /* count hi */
    legacy[pos++] = 0x01U;                                  /* count lo -> 1 */
    legacy[pos++] = (uint8_t)((DTC_1 >> 16U) & 0xFFU);
    legacy[pos++] = (uint8_t)((DTC_1 >>  8U) & 0xFFU);
    legacy[pos++] = (uint8_t)( DTC_1         & 0xFFU);
    legacy[pos++] = 0x09U;                                  /* legacy status byte */

    zassert_equal(UDS_STATUS_OK,
                  nvm_store_write(NVM_KEY_DTC_MIRROR, legacy, pos),
                  "must be able to write a legacy-format record directly");

    (void)dtc_mirror_init();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");

    uds_status_t rc = dtc_mirror_load();

    zassert_equal(UDS_STATUS_OK, rc,
                  "MIGRATION: a legacy (pre-#114) record must load as OK, not corrupt");
    zassert_equal(0x00U, dtc_database_find(DTC_1)->status_byte,
                  "MIGRATION: legacy record content must be discarded, not reinterpreted");
}

/* TC-MIRROR-027  Valid magic but an unsupported/foreign version byte →
 * load() reports ERR_NVM_DATA_CORRUPT (cannot safely reinterpret a layout
 * from a version this build does not know). */
ZTEST(dtc_mirror, test_load_reports_corrupt_on_bad_version)
{
    uint8_t raw[DTC_MIRROR_HEADER_BYTES];

    raw[0] = DTC_MIRROR_MAGIC_0;
    raw[1] = DTC_MIRROR_MAGIC_1;
    raw[2] = (uint8_t)0xFFU; /* unsupported version */
    raw[3] = 0x00U;
    raw[4] = 0x00U;

    zassert_equal(UDS_STATUS_OK,
                  nvm_store_write(NVM_KEY_DTC_MIRROR, raw, sizeof(raw)),
                  "must be able to write the record");

    (void)dtc_mirror_init();
    (void)dtc_database_register(DTC_1, 0x00U, "DTC_1");

    uds_status_t rc = dtc_mirror_load();

    zassert_equal(UDS_STATUS_ERR_NVM_DATA_CORRUPT, rc,
                  "unsupported version under a valid magic must be reported as corrupt");
}

/* ==========================================================================
 * Mirror sizing vs. the NVM record cap (issue #123)
 * ========================================================================== */

/* TC-MIRROR-028  DTC_MIRROR_MAX_BYTES (derived from today's
 * DTC_MIRROR_MAX_PERSISTED_DTCS) fits within NVM_MAX_RECORD_BYTES.
 *
 * The build-time _Static_assert in dtc_mirror.c is the primary enforcement
 * (it fails the BUILD, not just this test, if a future change to any
 * sizing constant breaks the inequality) — this is a runtime cross-check
 * of the same fact, and pins the exact byte counts so a silent change to
 * either constant is caught here too. */
ZTEST(dtc_mirror, test_mirror_max_bytes_fits_nvm_cap)
{
    zassert_equal(125U, (unsigned int)DTC_MIRROR_MAX_PERSISTED_DTCS,
                  "issue #123 ceiling: 5 + N*4 + 4 <= 512 => N <= 125");
    zassert_equal(509U, (unsigned int)DTC_MIRROR_MAX_BYTES,
                  "DTC_MIRROR_MAX_BYTES must be 5 + 125*4 + 4 = 509");
    zassert_true(DTC_MIRROR_MAX_BYTES <= NVM_MAX_RECORD_BYTES,
                  "DTC_MIRROR_MAX_BYTES must fit within NVM_MAX_RECORD_BYTES");
}

/* TC-MIRROR-029  Proves issue #123 was a real bug: recompute the OLD
 * (pre-fix) sizing formula directly from the raw wire-format constants —
 * header + UDS_MAX_DTC_COUNT (128) entries + CRC trailer, exactly what
 * DTC_MIRROR_MAX_BYTES used to expand to before this fix — and show it
 * exceeds NVM_MAX_RECORD_BYTES. This is independent of
 * DTC_MIRROR_MAX_PERSISTED_DTCS, so it proves the defect was real rather
 * than merely that today's cap is internally consistent (TC-MIRROR-028). */
ZTEST(dtc_mirror, test_old_full_table_sizing_would_have_exceeded_nvm_cap)
{
    size_t old_formula_bytes = (size_t)DTC_MIRROR_HEADER_BYTES
                              + ((size_t)UDS_MAX_DTC_COUNT * (size_t)DTC_MIRROR_ENTRY_BYTES)
                              + (size_t)DTC_MIRROR_CRC_BYTES;

    zassert_equal((size_t)521U, old_formula_bytes,
                  "pre-#123 formula: 5 + 128*4 + 4 = 521");
    zassert_true(old_formula_bytes > (size_t)NVM_MAX_RECORD_BYTES,
                  "issue #123: sizing the mirror from UDS_MAX_DTC_COUNT (128) "
                  "overshoots NVM_MAX_RECORD_BYTES (512) by 9 bytes — this is "
                  "the bug that made nvm_store_write() silently reject the "
                  "mirror write once the live DTC table grew past 125 entries");
}

/* ==========================================================================
 * run_all_tests
 * ========================================================================== */

void run_all_tests(void)
{
    /* dtc_mirror_init() */
    RUN_TEST(dtc_mirror__test_init_first_call_ok);
    RUN_TEST(dtc_mirror__test_is_ready_true_after_init);
    RUN_TEST(dtc_mirror__test_init_second_call_returns_already_initialized);
    RUN_TEST(dtc_mirror__test_is_ready_false_before_init);

    /* dtc_mirror_load() */
    RUN_TEST(dtc_mirror__test_load_before_init_not_initialized);
    RUN_TEST(dtc_mirror__test_load_nvm_not_ready_soft_degrade);
    RUN_TEST(dtc_mirror__test_load_first_boot_no_nvm_record);
    RUN_TEST(dtc_mirror__test_load_restores_status_bytes);
    RUN_TEST(dtc_mirror__test_load_skips_unregistered_codes);
    RUN_TEST(dtc_mirror__test_load_idempotent);

    /* dtc_mirror_flush_all() */
    RUN_TEST(dtc_mirror__test_flush_all_before_init);
    RUN_TEST(dtc_mirror__test_flush_all_nvm_not_ready);
    RUN_TEST(dtc_mirror__test_flush_all_writes_correct_header);
    RUN_TEST(dtc_mirror__test_flush_all_empty_table_writes_zero_count);

    /* dtc_mirror_save_one() */
    RUN_TEST(dtc_mirror__test_save_one_before_init);
    RUN_TEST(dtc_mirror__test_save_one_triggers_flush);
    RUN_TEST(dtc_mirror__test_save_one_after_clear_persists);

    /* dtc_mirror_clear_all() */
    RUN_TEST(dtc_mirror__test_clear_all_before_init);
    RUN_TEST(dtc_mirror__test_clear_all_writes_zeros_to_nvm);
    RUN_TEST(dtc_mirror__test_clear_all_then_load_in_same_session);

    /* H3 fix integration */
    RUN_TEST(dtc_mirror__test_h3_fix_correct_call_order);
    RUN_TEST(dtc_mirror__test_h3_fix_soft_degrade_nvm_not_ready);
    RUN_TEST(dtc_mirror__test_h3_fix_full_persistence_scenario);

    /* Integrity checking (issue #114) */
    RUN_TEST(dtc_mirror__test_load_reports_corrupt_on_bad_crc);
    RUN_TEST(dtc_mirror__test_load_reports_corrupt_on_truncated_record);
    RUN_TEST(dtc_mirror__test_load_treats_legacy_record_as_no_mirror);
    RUN_TEST(dtc_mirror__test_load_reports_corrupt_on_bad_version);

    /* Mirror sizing vs. the NVM record cap (issue #123) */
    RUN_TEST(dtc_mirror__test_mirror_max_bytes_fits_nvm_cap);
    RUN_TEST(dtc_mirror__test_old_full_table_sizing_would_have_exceeded_nvm_cap);
}
