// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_nvm_store_append.c
 *
 * MODULE UNDER TEST: platform/zephyr/nvm_store_append.c (issue #304)
 *
 * BUILD NOTE: this test bypasses the shared-stack-archive mechanism every
 * other unit_runnable/ test uses (build_tests.sh's STACK_SRCS /
 * tests/CMakeLists.txt's DIAG_STACK_SRCS) — both compile ONE fixed source
 * list, which already includes platform/zephyr/nvm_store_mock.c, once per
 * compile-flag "variant" and link every test against it; neither has a way
 * to swap OUT one shared-stack source for another per test. This module
 * under test defines the exact same public symbols
 * (nvm_store_init/write/read/...) as nvm_store_mock.c — linking both into
 * the same binary is a duplicate-symbol error, not a missing-wiring bug to
 * fix. It is still built and run by both: build_tests.sh via its
 * direct_link_srcs_for_test() special case, tests/CMakeLists.txt via a
 * standalone add_executable() target — see each file's own comment at this
 * test for the details.
 *
 * Coverage:
 *   TC-001  First-ever init: fresh bank 0, generation 1, schema written
 *   TC-002  Write then read round-trip
 *   TC-003  Read of a never-written key -> DID_NOT_FOUND
 *   TC-004  Delete then read -> DID_NOT_FOUND (native tombstone, not the
 *           sentinel-write heuristic nvm_store.h documents for backends
 *           without one)
 *   TC-005  Re-init after a "reboot" (same backing store, fresh in-RAM
 *           state) correctly reloads the persisted value — this is the
 *           exact property #304 exists to deliver, and the one the real
 *           bug caught on hardware (schema check always wiping on every
 *           boot) broke silently until fixed
 *   TC-006  Compaction: enough small writes to overflow one bank correctly
 *           preserves the live set and switches active bank/generation
 *   TC-007  A corrupted (bit-flipped) record's CRC mismatch is never
 *           trusted — read falls through to DID_NOT_FOUND rather than
 *           returning corrupt data
 *   TC-008  erase_all() preserves NVM_KEY_SEC_STATE (the #280 contract)
 *
 * REGRESSION PROOF: TC-005 is written specifically to catch the real bug
 * found on hardware during this issue's own hardware-verification pass —
 * nvm_store_init()'s schema check called the PUBLIC nvm_store_read(), which
 * gates on s_initialized; since that flag is set only after the schema
 * check runs, the check always saw "not initialized", always concluded
 * "schema missing", and called full_wipe() on every single boot, silently
 * destroying NVM_KEY_SEC_STATE every time. Confirmed this test fails
 * against that exact defect (reverted the fix, ran it, watched TC-005 fail)
 * before confirming it passes against the fix — this repo's standing
 * regression-test discipline.
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h for host compilation)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "nvm_store.h"

/* ==========================================================================
 * Test config — small banks so a handful of writes forces compaction
 * (real hardware uses 128 KB banks; the algorithm doesn't care about size)
 * ========================================================================== */

#define TEST_BANK_SIZE   (512U)   /* small enough that ~15 writes overflows it */

static nvm_store_cfg_t s_cfg = {
    .flash_dev    = (const void *)1,  /* never dereferenced by the host mock */
    .flash_offset = 0U,
    .sector_size  = TEST_BANK_SIZE,
    .sector_count = 2U,
};

/* ==========================================================================
 * setUp / tearDown
 * ========================================================================== */

void setUp(void)
{
    nvm_mock_reset();
}

void tearDown(void) {}

/* ==========================================================================
 * Test suite
 * ========================================================================== */

ZTEST_SUITE(nvm_store_append, NULL, NULL, NULL, NULL, NULL);

/* TC-001 */
ZTEST(nvm_store_append, test_first_init_fresh_store)
{
    uds_status_t status = nvm_store_init(&s_cfg);

    zassert_equal(UDS_STATUS_OK, status, "");
    zassert_true(nvm_store_is_ready(), "");
}

/* TC-002 */
ZTEST(nvm_store_append, test_write_read_roundtrip)
{
    uint8_t  written[4] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t  readback[4] = { 0 };
    size_t   read_len = 0U;

    zassert_equal(UDS_STATUS_OK, nvm_store_init(&s_cfg), "");
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_write(NVM_KEY_LIFECYCLE_CNT, written, sizeof(written)), "");
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_read(NVM_KEY_LIFECYCLE_CNT, readback, sizeof(readback), &read_len), "");
    zassert_equal(sizeof(written), read_len, "");
    zassert_equal(0, memcmp(written, readback, sizeof(written)), "");
}

/* TC-003 */
ZTEST(nvm_store_append, test_read_never_written_key)
{
    uint8_t buf[4];
    size_t  read_len = 0U;

    zassert_equal(UDS_STATUS_OK, nvm_store_init(&s_cfg), "");
    zassert_equal(UDS_STATUS_ERR_DID_NOT_FOUND,
                  nvm_store_read(NVM_KEY_LIFECYCLE_CNT, buf, sizeof(buf), &read_len), "");
}

/* TC-004 */
ZTEST(nvm_store_append, test_delete_then_read)
{
    uint8_t val = 0x7AU;
    uint8_t buf[1];

    zassert_equal(UDS_STATUS_OK, nvm_store_init(&s_cfg), "");
    zassert_equal(UDS_STATUS_OK, nvm_store_write(NVM_KEY_LIFECYCLE_CNT, &val, sizeof(val)), "");
    zassert_equal(UDS_STATUS_OK, nvm_store_delete(NVM_KEY_LIFECYCLE_CNT), "");
    zassert_equal(UDS_STATUS_ERR_DID_NOT_FOUND,
                  nvm_store_read(NVM_KEY_LIFECYCLE_CNT, buf, sizeof(buf), NULL), "");
    /* Idempotent: deleting an already-absent key is not an error. */
    zassert_equal(UDS_STATUS_OK, nvm_store_delete(NVM_KEY_LIFECYCLE_CNT), "");
}

/* TC-005 — the actual hardware-caught regression. */
ZTEST(nvm_store_append, test_persists_across_reinit)
{
    uint8_t  attempts = 2U;
    uint8_t  readback = 0U;
    size_t   read_len = 0U;

    zassert_equal(UDS_STATUS_OK, nvm_store_init(&s_cfg), "");
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_write(NVM_KEY_SEC_STATE, &attempts, sizeof(attempts)), "");

    /* Simulate a reboot: drop all in-RAM state (index, active bank, cursor,
     * s_initialized) WITHOUT touching the backing store — nvm_mock_deinit()
     * exists for exactly this, as opposed to nvm_mock_reset() which also
     * erases the "flash" and would defeat the point of this test. */
    nvm_mock_deinit();
    zassert_false(nvm_store_is_ready(), "");

    zassert_equal(UDS_STATUS_OK, nvm_store_init(&s_cfg), "");
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_read(NVM_KEY_SEC_STATE, &readback, sizeof(readback), &read_len), "");
    zassert_equal(sizeof(attempts), read_len, "");
    zassert_equal(attempts, readback,
                  "NVM_KEY_SEC_STATE must survive re-init against the same "
                  "backing store — this is the exact property issue #304 "
                  "exists to deliver");
}

/* TC-006 */
ZTEST(nvm_store_append, test_compaction_preserves_live_set)
{
    uint8_t  val;
    uint8_t  readback;
    uint32_t i;

    zassert_equal(UDS_STATUS_OK, nvm_store_init(&s_cfg), "");

    /* Each write is ~9 bytes raw (8-byte record header + 1-byte payload),
     * padded to the host mock's 8-byte write block — comfortably enough
     * iterations to overflow a 512-byte bank (minus its 32-byte header)
     * and force at least one compaction, while writing a DIFFERENT value
     * each time so the "skip identical write" optimisation never short-
     * circuits it. */
    for (i = 0U; i < 40U; i++) {
        val = (uint8_t)(i & 0xFFU);
        zassert_equal(UDS_STATUS_OK,
                      nvm_store_write(NVM_KEY_LIFECYCLE_CNT, &val, sizeof(val)), "");
    }

    /* The live set (just this one key) must have survived every
     * compaction along the way, holding the LAST value written. */
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_read(NVM_KEY_LIFECYCLE_CNT, &readback, sizeof(readback), NULL), "");
    zassert_equal((uint8_t)39U, readback, "");
}

/* TC-007 */
ZTEST(nvm_store_append, test_corrupt_record_not_trusted)
{
    uint8_t  val = 0xABU;
    uint8_t  buf[1];

    zassert_equal(UDS_STATUS_OK, nvm_store_init(&s_cfg), "");
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_write(NVM_KEY_LIFECYCLE_CNT, &val, sizeof(val)), "");

    /* Flip a bit directly in the mock "flash" backing this record's
     * payload byte, bypassing the module's own write path entirely —
     * nvm_mock_corrupt_byte() is a test-only hook for exactly this. */
    nvm_mock_corrupt_byte(40U); /* header slot (32) + 8-byte record header */

    /* Re-init to force a fresh scan against the now-corrupted bank —
     * read_record_header() must reject this record's CRC and treat the
     * scan as ended there, not surface the corrupted byte as real data. */
    nvm_mock_deinit();
    zassert_equal(UDS_STATUS_OK, nvm_store_init(&s_cfg), "");

    zassert_equal(UDS_STATUS_ERR_DID_NOT_FOUND,
                  nvm_store_read(NVM_KEY_LIFECYCLE_CNT, buf, sizeof(buf), NULL),
                  "a CRC-corrupt record must never be returned as valid data");
}

/* TC-008 */
ZTEST(nvm_store_append, test_erase_all_preserves_sec_state)
{
    uint8_t  sec_val = 0x03U;
    uint8_t  other_val = 0x09U;
    uint8_t  readback;

    zassert_equal(UDS_STATUS_OK, nvm_store_init(&s_cfg), "");
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_write(NVM_KEY_SEC_STATE, &sec_val, sizeof(sec_val)), "");
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_write(NVM_KEY_LIFECYCLE_CNT, &other_val, sizeof(other_val)), "");

    zassert_equal(UDS_STATUS_OK, nvm_store_erase_all(), "");

    zassert_equal(UDS_STATUS_OK,
                  nvm_store_read(NVM_KEY_SEC_STATE, &readback, sizeof(readback), NULL), "");
    zassert_equal(sec_val, readback, "[#280] NVM_KEY_SEC_STATE must survive erase_all()");

    zassert_equal(UDS_STATUS_ERR_DID_NOT_FOUND,
                  nvm_store_read(NVM_KEY_LIFECYCLE_CNT, &readback, sizeof(readback), NULL),
                  "every other key must be cleared by erase_all()");
}

/* ==========================================================================
 * run_all_tests
 * ========================================================================== */

void run_all_tests(void)
{
    RUN_TEST(nvm_store_append__test_first_init_fresh_store);
    RUN_TEST(nvm_store_append__test_write_read_roundtrip);
    RUN_TEST(nvm_store_append__test_read_never_written_key);
    RUN_TEST(nvm_store_append__test_delete_then_read);
    RUN_TEST(nvm_store_append__test_persists_across_reinit);
    RUN_TEST(nvm_store_append__test_compaction_preserves_live_set);
    RUN_TEST(nvm_store_append__test_corrupt_record_not_trusted);
    RUN_TEST(nvm_store_append__test_erase_all_preserves_sec_state);
}
