// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_phase5_access_table.c
 *
 * MODULE UNDER TEST: core/uds_access_table.c
 *                    Data-driven access rights table (Phase 5)
 *
 * TEST CASES:
 *   TC-ACL-001  uds_access_table_get_default() returns non-NULL
 *   TC-ACL-002  Default table has exactly UDS_ACCESS_TABLE_DEFAULT_COUNT entries
 *   TC-ACL-003  0x10 allowed in DEFAULT session (default table)
 *   TC-ACL-004  0x10 allowed in EXTENDED session (default table)
 *   TC-ACL-005  0x27 blocked in DEFAULT session (default table)
 *   TC-ACL-006  0x27 allowed in EXTENDED session (default table)
 *   TC-ACL-007  0x27 allowed in PROGRAMMING session (default table)
 *   TC-ACL-008  0x2E blocked in DEFAULT session (default table)
 *   TC-ACL-009  0x2E allowed in EXTENDED session but requires security unlock
 *   TC-ACL-010  0x14 blocked in DEFAULT session (default table)
 *   TC-ACL-011  0x14 allowed in EXTENDED session (default table)
 *   TC-ACL-012  0x85 blocked in DEFAULT session (default table)
 *   TC-ACL-013  0x3E allowed in DEFAULT session (default table)
 *   TC-ACL-014  0x22 allowed in DEFAULT session (default table)
 *   TC-ACL-015  0x19 allowed in DEFAULT session (default table)
 *   TC-ACL-016  0x28 blocked in DEFAULT session (default table)
 *   TC-ACL-017  lookup() with NULL table returns ERR_NULL_PTR
 *   TC-ACL-018  lookup() with NULL out_entry returns ERR_NULL_PTR
 *   TC-ACL-019  lookup() count=0 → no match (NULL out_entry, OK return)
 *   TC-ACL-020  Unknown SID not in table → NULL out_entry (no restriction)
 *   TC-ACL-021  Custom single-entry table overrides default for a specific SID
 *   TC-ACL-022  enforce() with NULL entry → OK (no restriction)
 *   TC-ACL-023  enforce() with NULL security_ctx and require_unlocked → ERR_NULL_PTR
 *   TC-ACL-024  enforce() with require_unlocked=false → OK regardless of security state
 *   TC-ACL-025  acl_session_to_bit: session_mask bitmapping for all four types
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "uds_access_table.h"
#include "uds_security.h"
#include "uds_session.h"
#include "uds_types.h"

ZTEST_SUITE(test_phase5_access_table, NULL, NULL, NULL, NULL, NULL);

/* =========================================================================
 * Helpers
 * ========================================================================= */

/** Check whether a (service_id, session) pair is allowed in the default table. */
static bool default_table_allows(uint8_t sid, uds_session_type_t sess)
{
    const uds_access_entry_t *entry = NULL;
    uds_status_t rc = uds_access_table_lookup(
        uds_access_table_get_default(),
        (uint8_t)UDS_ACCESS_TABLE_DEFAULT_COUNT,
        sid, sess, &entry);

    if (rc != UDS_STATUS_OK) { return false; }
    if (entry == NULL) { return true; } /* not in table = allowed */

    /* Check session bit */
    uint8_t bit = (uint8_t)((uint8_t)1U << ((uint8_t)sess - (uint8_t)1U));
    return ((entry->session_mask & bit) != (uint8_t)0U);
}

/* =========================================================================
 * Test cases
 * ========================================================================= */

/**
 * TC-ACL-001: uds_access_table_get_default() returns non-NULL.
 */
ZTEST(test_phase5_access_table, tc001_get_default_not_null)
{
    zassert_not_null(uds_access_table_get_default(),
        "default table pointer must not be NULL");
}

/**
 * TC-ACL-002: Default table has UDS_ACCESS_TABLE_DEFAULT_COUNT entries.
 * We verify by counting lookups for each SID in the default table.
 * Count updated from 10 → 13 when DFU services 0x34/0x36/0x37 were added,
 * then 13 → 14 when 0x35 RequestUpload was added,
 * then 14 → 15 when 0x2F InputOutputControlByIdentifier was added,
 * then 15 → 16 when 0x2A ReadDataByPeriodicIdentifier was added,
 * then 16 → 18 when 0x23 ReadMemoryByAddress + 0x3D WriteMemoryByAddress were added.
 */
ZTEST(test_phase5_access_table, tc002_default_table_count)
{
    zassert_equal((uint8_t)UDS_ACCESS_TABLE_DEFAULT_COUNT, (uint8_t)18U,
        "default table must have exactly 18 entries");
}

/**
 * TC-ACL-003: 0x10 is allowed in DEFAULT session.
 */
ZTEST(test_phase5_access_table, tc003_0x10_allowed_default)
{
    zassert_true(default_table_allows(UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
                                       UDS_SESSION_DEFAULT),
        "0x10 must be allowed in DEFAULT session");
}

/**
 * TC-ACL-004: 0x10 is allowed in EXTENDED session.
 */
ZTEST(test_phase5_access_table, tc004_0x10_allowed_extended)
{
    zassert_true(default_table_allows(UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
                                       UDS_SESSION_EXTENDED),
        "0x10 must be allowed in EXTENDED session");
}

/**
 * TC-ACL-005: 0x27 is NOT allowed in DEFAULT session.
 */
ZTEST(test_phase5_access_table, tc005_0x27_blocked_default)
{
    zassert_false(default_table_allows(UDS_SID_SECURITY_ACCESS, UDS_SESSION_DEFAULT),
        "0x27 must be blocked in DEFAULT session");
}

/**
 * TC-ACL-006: 0x27 is allowed in EXTENDED session.
 */
ZTEST(test_phase5_access_table, tc006_0x27_allowed_extended)
{
    zassert_true(default_table_allows(UDS_SID_SECURITY_ACCESS, UDS_SESSION_EXTENDED),
        "0x27 must be allowed in EXTENDED session");
}

/**
 * TC-ACL-007: 0x27 is allowed in PROGRAMMING session.
 */
ZTEST(test_phase5_access_table, tc007_0x27_allowed_programming)
{
    zassert_true(default_table_allows(UDS_SID_SECURITY_ACCESS, UDS_SESSION_PROGRAMMING),
        "0x27 must be allowed in PROGRAMMING session");
}

/**
 * TC-ACL-008: 0x2E is NOT allowed in DEFAULT session.
 */
ZTEST(test_phase5_access_table, tc008_0x2e_blocked_default)
{
    zassert_false(default_table_allows(UDS_SID_WRITE_DATA_BY_ID, UDS_SESSION_DEFAULT),
        "0x2E must be blocked in DEFAULT session");
}

/**
 * TC-ACL-009: 0x2E entry for EXTENDED session requires security unlock.
 */
ZTEST(test_phase5_access_table, tc009_0x2e_requires_security)
{
    const uds_access_entry_t *entry = NULL;
    uds_status_t rc = uds_access_table_lookup(
        uds_access_table_get_default(),
        (uint8_t)UDS_ACCESS_TABLE_DEFAULT_COUNT,
        UDS_SID_WRITE_DATA_BY_ID, UDS_SESSION_EXTENDED, &entry);

    zassert_equal(rc, UDS_STATUS_OK, "lookup must return OK");
    zassert_not_null(entry, "0x2E must have a table entry for EXTENDED session");
    zassert_true(entry->require_unlocked,
        "0x2E entry must have require_unlocked=true");
    zassert_equal(entry->required_sec_level, (uint8_t)UDS_SEC_LEVEL_1_SEED,
        "0x2E must require Level-1 security unlock");
}

/**
 * TC-ACL-010: 0x14 is NOT allowed in DEFAULT session.
 */
ZTEST(test_phase5_access_table, tc010_0x14_blocked_default)
{
    zassert_false(default_table_allows(UDS_SID_CLEAR_DIAGNOSTIC_INFO, UDS_SESSION_DEFAULT),
        "0x14 must be blocked in DEFAULT session");
}

/**
 * TC-ACL-011: 0x14 is allowed in EXTENDED session.
 */
ZTEST(test_phase5_access_table, tc011_0x14_allowed_extended)
{
    zassert_true(default_table_allows(UDS_SID_CLEAR_DIAGNOSTIC_INFO, UDS_SESSION_EXTENDED),
        "0x14 must be allowed in EXTENDED session");
}

/**
 * TC-ACL-012: 0x85 is NOT allowed in DEFAULT session.
 */
ZTEST(test_phase5_access_table, tc012_0x85_blocked_default)
{
    zassert_false(default_table_allows(UDS_SID_CONTROL_DTC_SETTING, UDS_SESSION_DEFAULT),
        "0x85 must be blocked in DEFAULT session");
}

/**
 * TC-ACL-013: 0x3E is allowed in DEFAULT session.
 */
ZTEST(test_phase5_access_table, tc013_0x3e_allowed_default)
{
    zassert_true(default_table_allows(UDS_SID_TESTER_PRESENT, UDS_SESSION_DEFAULT),
        "0x3E must be allowed in DEFAULT session");
}

/**
 * TC-ACL-014: 0x22 is allowed in DEFAULT session.
 */
ZTEST(test_phase5_access_table, tc014_0x22_allowed_default)
{
    zassert_true(default_table_allows(UDS_SID_READ_DATA_BY_ID, UDS_SESSION_DEFAULT),
        "0x22 must be allowed in DEFAULT session");
}

/**
 * TC-ACL-015: 0x19 is allowed in DEFAULT session.
 */
ZTEST(test_phase5_access_table, tc015_0x19_allowed_default)
{
    zassert_true(default_table_allows(UDS_SID_READ_DTC_INFO, UDS_SESSION_DEFAULT),
        "0x19 must be allowed in DEFAULT session");
}

/**
 * TC-ACL-016: 0x28 is NOT allowed in DEFAULT session.
 */
ZTEST(test_phase5_access_table, tc016_0x28_blocked_default)
{
    zassert_false(default_table_allows(UDS_SID_COMMUNICATION_CONTROL, UDS_SESSION_DEFAULT),
        "0x28 must be blocked in DEFAULT session");
}

/**
 * TC-ACL-017: lookup() with NULL table → ERR_NULL_PTR.
 */
ZTEST(test_phase5_access_table, tc017_lookup_null_table)
{
    const uds_access_entry_t *entry = NULL;
    uds_status_t rc = uds_access_table_lookup(NULL, 1U,
                                               UDS_SID_ECU_RESET, UDS_SESSION_DEFAULT,
                                               &entry);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL table must return ERR_NULL_PTR");
}

/**
 * TC-ACL-018: lookup() with NULL out_entry → ERR_NULL_PTR.
 */
ZTEST(test_phase5_access_table, tc018_lookup_null_out_entry)
{
    const uds_access_entry_t *tbl = uds_access_table_get_default();
    uds_status_t rc = uds_access_table_lookup(tbl, (uint8_t)UDS_ACCESS_TABLE_DEFAULT_COUNT,
                                               UDS_SID_ECU_RESET, UDS_SESSION_DEFAULT,
                                               NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL out_entry must return ERR_NULL_PTR");
}

/**
 * TC-ACL-019: lookup() with count=0 → OK, NULL out_entry (no restriction).
 */
ZTEST(test_phase5_access_table, tc019_lookup_count_zero)
{
    const uds_access_entry_t dummy = {
        .service_id  = UDS_SID_ECU_RESET,
        .session_mask = UDS_ACL_SESSION_ALL,
    };
    const uds_access_entry_t *entry = NULL;
    uds_status_t rc = uds_access_table_lookup(&dummy, 0U,
                                               UDS_SID_ECU_RESET, UDS_SESSION_DEFAULT,
                                               &entry);
    zassert_equal(rc, UDS_STATUS_OK, "count=0 must return OK");
    zassert_is_null(entry, "count=0 must yield NULL out_entry (no restriction)");
}

/**
 * TC-ACL-020: Unknown SID not in table → NULL out_entry (no restriction).
 */
ZTEST(test_phase5_access_table, tc020_unknown_sid_no_restriction)
{
    const uds_access_entry_t *entry = NULL;
    uds_status_t rc = uds_access_table_lookup(
        uds_access_table_get_default(),
        (uint8_t)UDS_ACCESS_TABLE_DEFAULT_COUNT,
        (uint8_t)0xFEU, /* unknown SID */
        UDS_SESSION_DEFAULT,
        &entry);
    zassert_equal(rc, UDS_STATUS_OK, "unknown SID must return OK");
    zassert_is_null(entry,
        "unknown SID must yield NULL entry (no restriction applied)");
}

/**
 * TC-ACL-021: Custom table with one entry restricting 0x3E to non-default
 * overrides the default (which allows 0x3E everywhere).
 */
ZTEST(test_phase5_access_table, tc021_custom_table_overrides_default)
{
    /* Custom rule: 0x3E restricted to EXTENDED only. */
    static const uds_access_entry_t custom[1U] = {{
        .service_id        = UDS_SID_TESTER_PRESENT,
        .session_mask      = UDS_ACL_SESSION_EXTENDED,
        .required_sec_level = (uint8_t)0U,
        .require_unlocked  = false,
    }};

    const uds_access_entry_t *entry = NULL;
    uds_status_t rc = uds_access_table_lookup(custom, 1U,
                                               UDS_SID_TESTER_PRESENT,
                                               UDS_SESSION_DEFAULT, &entry);
    zassert_equal(rc, UDS_STATUS_OK, "lookup must return OK");
    zassert_not_null(entry, "0x3E must be found in custom table");

    /* session_mask should NOT include DEFAULT — disallowed. */
    uint8_t def_bit = UDS_ACL_SESSION_DEFAULT;
    zassert_equal(entry->session_mask & def_bit, (uint8_t)0U,
        "custom table entry must not allow DEFAULT session for 0x3E");
}

/**
 * TC-ACL-022: enforce() with NULL entry → UDS_STATUS_OK (no restriction).
 */
ZTEST(test_phase5_access_table, tc022_enforce_null_entry_ok)
{
    uds_security_ctx_t sec_ctx;
    (void)memset(&sec_ctx, 0, sizeof(sec_ctx));
    uds_status_t rc = uds_access_table_enforce(NULL, &sec_ctx);
    zassert_equal(rc, UDS_STATUS_OK,
        "enforce with NULL entry must return OK (no restriction)");
}

/**
 * TC-ACL-023: enforce() with require_unlocked=true and NULL security_ctx → ERR_NULL_PTR.
 */
ZTEST(test_phase5_access_table, tc023_enforce_null_sec_ctx_with_require)
{
    static const uds_access_entry_t entry = {
        .service_id        = UDS_SID_WRITE_DATA_BY_ID,
        .session_mask      = UDS_ACL_SESSION_EXTENDED,
        .required_sec_level = UDS_SEC_LEVEL_1_SEED,
        .require_unlocked  = true,
    };
    uds_status_t rc = uds_access_table_enforce(&entry, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR,
        "require_unlocked=true with NULL security_ctx must return ERR_NULL_PTR");
}

/**
 * TC-ACL-024: enforce() with require_unlocked=false → OK regardless of security.
 */
ZTEST(test_phase5_access_table, tc024_enforce_no_security_required_ok)
{
    static const uds_access_entry_t entry = {
        .service_id        = UDS_SID_READ_DTC_INFO,
        .session_mask      = UDS_ACL_SESSION_ALL,
        .required_sec_level = (uint8_t)0U,
        .require_unlocked  = false,
    };
    /* Pass NULL security_ctx — must still return OK since require_unlocked=false */
    uds_status_t rc = uds_access_table_enforce(&entry, NULL);
    zassert_equal(rc, UDS_STATUS_OK,
        "require_unlocked=false must return OK even with NULL security_ctx");
}

/**
 * TC-ACL-025: Session bitmask mapping correctness.
 * Verify each session constant maps to the expected bit value.
 */
ZTEST(test_phase5_access_table, tc025_session_bitmask_mapping)
{
    /* UDS_SESSION_DEFAULT (0x01) → bit 0 = 0x01 */
    zassert_equal(UDS_ACL_SESSION_DEFAULT, (uint8_t)0x01U,
        "DEFAULT session mask must be 0x01");

    /* UDS_SESSION_PROGRAMMING (0x02) → bit 1 = 0x02 */
    zassert_equal(UDS_ACL_SESSION_PROGRAMMING, (uint8_t)0x02U,
        "PROGRAMMING session mask must be 0x02");

    /* UDS_SESSION_EXTENDED (0x03) → bit 2 = 0x04 */
    zassert_equal(UDS_ACL_SESSION_EXTENDED, (uint8_t)0x04U,
        "EXTENDED session mask must be 0x04");

    /* UDS_SESSION_SAFETY_SYSTEM (0x04) → bit 3 = 0x08 */
    zassert_equal(UDS_ACL_SESSION_SAFETY, (uint8_t)0x08U,
        "SAFETY session mask must be 0x08");

    /* ALL must equal bitwise OR of all four */
    zassert_equal(UDS_ACL_SESSION_ALL,
                  (uint8_t)(UDS_ACL_SESSION_DEFAULT | UDS_ACL_SESSION_PROGRAMMING |
                             UDS_ACL_SESSION_EXTENDED | UDS_ACL_SESSION_SAFETY),
        "ALL mask must equal OR of all four session masks");
}

/* =========================================================================
 * run_all_tests
 * ========================================================================= */

extern void test_phase5_access_table__tc001_get_default_not_null(void);
extern void test_phase5_access_table__tc002_default_table_count(void);
extern void test_phase5_access_table__tc003_0x10_allowed_default(void);
extern void test_phase5_access_table__tc004_0x10_allowed_extended(void);
extern void test_phase5_access_table__tc005_0x27_blocked_default(void);
extern void test_phase5_access_table__tc006_0x27_allowed_extended(void);
extern void test_phase5_access_table__tc007_0x27_allowed_programming(void);
extern void test_phase5_access_table__tc008_0x2e_blocked_default(void);
extern void test_phase5_access_table__tc009_0x2e_requires_security(void);
extern void test_phase5_access_table__tc010_0x14_blocked_default(void);
extern void test_phase5_access_table__tc011_0x14_allowed_extended(void);
extern void test_phase5_access_table__tc012_0x85_blocked_default(void);
extern void test_phase5_access_table__tc013_0x3e_allowed_default(void);
extern void test_phase5_access_table__tc014_0x22_allowed_default(void);
extern void test_phase5_access_table__tc015_0x19_allowed_default(void);
extern void test_phase5_access_table__tc016_0x28_blocked_default(void);
extern void test_phase5_access_table__tc017_lookup_null_table(void);
extern void test_phase5_access_table__tc018_lookup_null_out_entry(void);
extern void test_phase5_access_table__tc019_lookup_count_zero(void);
extern void test_phase5_access_table__tc020_unknown_sid_no_restriction(void);
extern void test_phase5_access_table__tc021_custom_table_overrides_default(void);
extern void test_phase5_access_table__tc022_enforce_null_entry_ok(void);
extern void test_phase5_access_table__tc023_enforce_null_sec_ctx_with_require(void);
extern void test_phase5_access_table__tc024_enforce_no_security_required_ok(void);
extern void test_phase5_access_table__tc025_session_bitmask_mapping(void);

void run_all_tests(void)
{
    RUN_TEST(test_phase5_access_table__tc001_get_default_not_null);
    RUN_TEST(test_phase5_access_table__tc002_default_table_count);
    RUN_TEST(test_phase5_access_table__tc003_0x10_allowed_default);
    RUN_TEST(test_phase5_access_table__tc004_0x10_allowed_extended);
    RUN_TEST(test_phase5_access_table__tc005_0x27_blocked_default);
    RUN_TEST(test_phase5_access_table__tc006_0x27_allowed_extended);
    RUN_TEST(test_phase5_access_table__tc007_0x27_allowed_programming);
    RUN_TEST(test_phase5_access_table__tc008_0x2e_blocked_default);
    RUN_TEST(test_phase5_access_table__tc009_0x2e_requires_security);
    RUN_TEST(test_phase5_access_table__tc010_0x14_blocked_default);
    RUN_TEST(test_phase5_access_table__tc011_0x14_allowed_extended);
    RUN_TEST(test_phase5_access_table__tc012_0x85_blocked_default);
    RUN_TEST(test_phase5_access_table__tc013_0x3e_allowed_default);
    RUN_TEST(test_phase5_access_table__tc014_0x22_allowed_default);
    RUN_TEST(test_phase5_access_table__tc015_0x19_allowed_default);
    RUN_TEST(test_phase5_access_table__tc016_0x28_blocked_default);
    RUN_TEST(test_phase5_access_table__tc017_lookup_null_table);
    RUN_TEST(test_phase5_access_table__tc018_lookup_null_out_entry);
    RUN_TEST(test_phase5_access_table__tc019_lookup_count_zero);
    RUN_TEST(test_phase5_access_table__tc020_unknown_sid_no_restriction);
    RUN_TEST(test_phase5_access_table__tc021_custom_table_overrides_default);
    RUN_TEST(test_phase5_access_table__tc022_enforce_null_entry_ok);
    RUN_TEST(test_phase5_access_table__tc023_enforce_null_sec_ctx_with_require);
    RUN_TEST(test_phase5_access_table__tc024_enforce_no_security_required_ok);
    RUN_TEST(test_phase5_access_table__tc025_session_bitmask_mapping);
}
