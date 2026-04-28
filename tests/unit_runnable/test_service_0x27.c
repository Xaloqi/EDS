// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit/test_service_0x27.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x27.c
 *                    SID 0x27 — SecurityAccess
 *
 * PURPOSE:
 *   Verify the complete seed/key exchange orchestration, NRC mapping, and
 *   lockout enforcement as implemented by the 0x27 handler + security module.
 *
 * TEST CASES:
 *   TC-0x27-001  Seed request (subFn 0x01) → 0x67 0x01 + seed bytes
 *   TC-0x27-002  Seed request carries 4 seed bytes (UDS_SECURITY_SEED_LEN)
 *   TC-0x27-003  Response length = 2 + seed_len
 *   TC-0x27-004  Already-unlocked seed request → 0x67 0x01 + 4 zero bytes
 *   TC-0x27-005  Correct key (subFn 0x02) → 0x67 0x02, length 2
 *   TC-0x27-006  Wrong key → ERR_SEC_INVALID_KEY (→ NRC 0x35)
 *   TC-0x27-007  Lockout after 3 consecutive wrong keys → ERR_SEC_ATTEMPT_EXCEEDED
 *   TC-0x27-008  SendKey without prior seed → ERR_SEC_SEED_UNAVAILABLE (NRC 0x24 requestSequenceError)
 *   TC-0x27-009  Seed request with length 1 (SID only) → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x27-010  Key request with length 2 (no key bytes) → ERR_INVALID_PARAM
 *   TC-0x27-011  Odd sub-function 0x03 (level 2 seed) → 0x67 0x03 + seed
 *   TC-0x27-012  Even sub-function 0x04 (level 2 key) → accepted as key path
 *   TC-0x27-013  NULL req  → ERR_NULL_PTR
 *   TC-0x27-014  NULL resp → ERR_NULL_PTR
 *   TC-0x27-015  Lockout expiry: after lockout_ms ticks, seed request succeeds
 *
 * FRAMEWORK: Zephyr Ztest
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "services.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_security_algo.h"  /* UDS_ALGO_KEY_LEN */
#include "uds_types.h"

/* =========================================================================
 * Key algorithm stubs
 * ========================================================================= */

static uds_status_t t_seed(uint8_t l, uint8_t *b, uint8_t n, uint8_t *o)
{
    /* [P1-SEC FIX] Fill the full UDS_SECURITY_SEED_LEN (8) bytes.
     * The mock fills as many bytes as the buffer allows up to the seed
     * length so that response-length assertions pass after the seed size
     * was increased from 4 to 8 in Phase 1. */
    (void)l;
    uint8_t fill = (n < (uint8_t)UDS_SECURITY_SEED_LEN) ? n
                                                         : (uint8_t)UDS_SECURITY_SEED_LEN;
    for (uint8_t i = 0U; i < fill; i++) {
        b[i] = (uint8_t)(0x10U + i);
    }
    *o = fill;
    return UDS_STATUS_OK;
}

static bool t_key(uint8_t l, const uint8_t *s, uint8_t sl,
                  const uint8_t *k, uint8_t kl)
{
    /* [P1-SEC FIX] Seed (sl=8) and key (kl=4) lengths differ after Phase 1.
     * Validate kl bytes of key against the first kl bytes of seed.
     * sl must be >= kl; if sl < kl the key covers more bytes than the seed,
     * which is an implementation error. */
    (void)l;
    if ((sl == 0U) || (kl == 0U) || (sl < kl)) return false;
    for (uint8_t i = 0U; i < kl; i++) {
        if (k[i] != (uint8_t)(s[i] ^ 0xAAU)) return false;
    }
    return true;
}

/* =========================================================================
 * Context
 * ========================================================================= */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;

/* Short lockout_ms (10) so expiry tests run fast. */
static void setup(void)
{
    memset(&g_sess, 0, sizeof(g_sess));
    memset(&g_sec,  0, sizeof(g_sec));
    memset(&g_srv,  0, sizeof(g_srv));

    uds_session_init(&g_sess, 5000U);
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED); /* 0x27 needs EXTENDED */

    static const uds_security_cfg_t sc = {
        .max_attempts     = 3U,
        .lockout_ms       = 10U,   /* short for test speed */
        .key_validate_cb  = t_key,
        .seed_generate_cb = t_seed,
    };
    uds_security_init(&g_sec, &sc);

    static const uds_server_cfg_t svc = {
        .p2_server_max_ms      = 25U,
        .p2_star_server_max_ms = 5000U,
        .session_ctx           = &g_sess,
        .security_ctx          = &g_sec,
        .service_table         = g_uds_service_table,
        .service_table_count   = (uint8_t)UDS_SERVICE_TABLE_COUNT,
    };
    uds_server_init(&g_srv, &svc);
}

/** Build a seed-request PDU. */
static uds_msg_buf_t make_seed_req(uint8_t sub_fn)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = 0x27U;
    r.data[1] = sub_fn;
    r.length  = 2U;
    return r;
}

/** Build a key-submission PDU with the given key bytes. */
static uds_msg_buf_t make_key_req(uint8_t sub_fn, const uint8_t *key, uint8_t klen)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = 0x27U;
    r.data[1] = sub_fn;
    memcpy(&r.data[2], key, klen);
    r.length  = (uint16_t)(2U + klen);
    return r;
}

/** Extract the 4-byte seed from a 0x67 response. */
static void get_seed_from_resp(const uds_msg_buf_t *resp, uint8_t *seed_out)
{
    memcpy(seed_out, &resp->data[2], UDS_SECURITY_SEED_LEN);
}

/** Compute the correct key from seed (XOR 0xAA over first UDS_ALGO_KEY_LEN bytes).
 *
 * [P1-SEC FIX] The key length (UDS_ALGO_KEY_LEN = 4) is independent of the
 * seed length (UDS_SECURITY_SEED_LEN = 8). The t_key mock and service handler
 * both compare kl bytes of key against sl bytes of seed; we derive exactly
 * UDS_ALGO_KEY_LEN key bytes from the first UDS_ALGO_KEY_LEN seed bytes.
 */
static void compute_key(const uint8_t *seed, uint8_t *key_out)
{
    for (uint8_t i = 0U; i < (uint8_t)UDS_ALGO_KEY_LEN; i++) {
        key_out[i] = (uint8_t)(seed[i] ^ 0xAAU);
    }
}

/** Full seed→key unlock sequence; returns key-response status.
 * [P1-SEC FIX] key buffer and make_key_req use UDS_ALGO_KEY_LEN (4), not
 * UDS_SECURITY_SEED_LEN (8).  Seed is 8 bytes; key derived from first 4. */
static uds_status_t do_unlock(uint8_t seed_sfn, uint8_t key_sfn)
{
    uds_msg_buf_t seed_resp = {0};
    uds_msg_buf_t seed_req  = make_seed_req(seed_sfn);
    uds_service_0x27_handler(&g_srv, &seed_req, &seed_resp);

    uint8_t seed[UDS_SECURITY_SEED_LEN];
    get_seed_from_resp(&seed_resp, seed);

    uint8_t key[UDS_ALGO_KEY_LEN];
    compute_key(seed, key);

    uds_msg_buf_t key_req  = make_key_req(key_sfn, key, (uint8_t)UDS_ALGO_KEY_LEN);
    uds_msg_buf_t key_resp = {0};
    return uds_service_0x27_handler(&g_srv, &key_req, &key_resp);
}

/* =========================================================================
 * Test suite
 * ========================================================================= */

ZTEST_SUITE(test_service_0x27, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-0x27-001: Seed request subFn 0x01 → response 0x67 0x01 + seed.
 */
ZTEST(test_service_0x27, tc001_seed_request_ok)
{
    setup();
    uds_msg_buf_t req  = make_seed_req(0x01U);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x27_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,   "Seed request must succeed");
    zassert_equal(resp.data[0], 0x67U, "Response SID must be 0x67");
    zassert_equal(resp.data[1], 0x01U, "Sub-function echoed as 0x01");
}

/**
 * TC-0x27-002: Seed response carries UDS_SECURITY_SEED_LEN (4) seed bytes.
 */
ZTEST(test_service_0x27, tc002_seed_length)
{
    setup();
    uds_msg_buf_t req  = make_seed_req(0x01U);
    uds_msg_buf_t resp = {0};

    uds_service_0x27_handler(&g_srv, &req, &resp);

    /* Response = [0x67, subFn, seed0, seed1, seed2, seed3] */
    zassert_equal(resp.length, (uint16_t)(2U + UDS_SECURITY_SEED_LEN),
                  "Response length must be 2 + seed_len");
}

/**
 * TC-0x27-003: Response total length == 2 + seed_len.
 */
ZTEST(test_service_0x27, tc003_response_length)
{
    setup();
    uds_msg_buf_t req  = make_seed_req(0x01U);
    uds_msg_buf_t resp = {0};

    uds_service_0x27_handler(&g_srv, &req, &resp);

    zassert_equal(resp.length, 2U + UDS_SECURITY_SEED_LEN,
                  "Seed response length must be 2 + 4 = 6");
}

/**
 * TC-0x27-004: Already-unlocked seed request → response with 4 zero bytes.
 * ISO 14229-1: A seed of all zeros means "already unlocked at this level".
 */
ZTEST(test_service_0x27, tc004_already_unlocked_zero_seed)
{
    setup();
    /* Unlock first */
    do_unlock(0x01U, 0x02U);

    /* Request seed again for level 1 */
    uds_msg_buf_t req  = make_seed_req(0x01U);
    uds_msg_buf_t resp = {0};
    uds_service_0x27_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[0], 0x67U, "Response SID must be 0x67");
    for (uint8_t i = 0; i < UDS_SECURITY_SEED_LEN; i++) {
        zassert_equal(resp.data[2U + i], 0x00U,
                      "Seed byte %d must be 0x00 when already unlocked", i);
    }
}

/**
 * TC-0x27-005: Correct key (subFn 0x02) → 0x67 0x02, length 2.
 */
ZTEST(test_service_0x27, tc005_correct_key_unlocks)
{
    setup();

    /* Step 1: Get seed */
    uds_msg_buf_t seed_req  = make_seed_req(0x01U);
    uds_msg_buf_t seed_resp = {0};
    uds_service_0x27_handler(&g_srv, &seed_req, &seed_resp);

    /* Step 2: Compute and send correct key.
     * [P1-SEC FIX] key is UDS_ALGO_KEY_LEN (4) bytes, seed is 8. */
    uint8_t seed[UDS_SECURITY_SEED_LEN];
    get_seed_from_resp(&seed_resp, seed);
    uint8_t key[UDS_ALGO_KEY_LEN];
    compute_key(seed, key);

    uds_msg_buf_t key_req  = make_key_req(0x02U, key, (uint8_t)UDS_ALGO_KEY_LEN);
    uds_msg_buf_t key_resp = {0};

    uds_status_t rc = uds_service_0x27_handler(&g_srv, &key_req, &key_resp);

    zassert_equal(rc, UDS_STATUS_OK,   "Correct key must unlock");
    zassert_equal(key_resp.data[0], 0x67U, "Response SID must be 0x67");
    zassert_equal(key_resp.data[1], 0x02U, "Sub-function echoed as 0x02");
    zassert_equal(key_resp.length,  2U,    "Key response must be 2 bytes");
}

/**
 * TC-0x27-006: Wrong key → ERR_SEC_INVALID_KEY (→ NRC 0x35).
 */
ZTEST(test_service_0x27, tc006_wrong_key_rejected)
{
    setup();

    /* Get seed */
    uds_msg_buf_t seed_req  = make_seed_req(0x01U);
    uds_msg_buf_t seed_resp = {0};
    uds_service_0x27_handler(&g_srv, &seed_req, &seed_resp);

    /* Send deliberately wrong key (4 bytes = UDS_ALGO_KEY_LEN). */
    uint8_t bad_key[UDS_ALGO_KEY_LEN] = { 0xDEU, 0xADU, 0xBEU, 0xEFU };
    uds_msg_buf_t key_req  = make_key_req(0x02U, bad_key, (uint8_t)UDS_ALGO_KEY_LEN);
    uds_msg_buf_t key_resp = {0};

    uds_status_t rc = uds_service_0x27_handler(&g_srv, &key_req, &key_resp);

    zassert_equal(rc, UDS_STATUS_ERR_SEC_INVALID_KEY,
                  "Wrong key must return ERR_SEC_INVALID_KEY");
}

/**
 * TC-0x27-007: 3 consecutive wrong keys → ERR_SEC_ATTEMPT_EXCEEDED (NRC 0x36).
 */
ZTEST(test_service_0x27, tc007_lockout_after_max_attempts)
{
    setup();

    /* [P1-SEC FIX] key buffer = UDS_ALGO_KEY_LEN (4 bytes) */
    uint8_t bad_key[UDS_ALGO_KEY_LEN] = { 0xFFU, 0xFFU, 0xFFU, 0xFFU };
    uds_status_t last_rc = UDS_STATUS_OK;

    for (int attempt = 0; attempt < 4; attempt++) {
        uds_msg_buf_t seed_req  = make_seed_req(0x01U);
        uds_msg_buf_t seed_resp = {0};
        uds_status_t  sr = uds_service_0x27_handler(&g_srv, &seed_req, &seed_resp);

        if (sr != UDS_STATUS_OK) {
            last_rc = sr;
            break;
        }

        uds_msg_buf_t key_req  = make_key_req(0x02U, bad_key, (uint8_t)UDS_ALGO_KEY_LEN);
        uds_msg_buf_t key_resp = {0};
        last_rc = uds_service_0x27_handler(&g_srv, &key_req, &key_resp);

        if (last_rc == UDS_STATUS_ERR_SEC_ATTEMPT_EXCEEDED) {
            break;
        }
    }

    zassert_equal(last_rc, UDS_STATUS_ERR_SEC_ATTEMPT_EXCEEDED,
                  "After max wrong keys, must return ERR_SEC_ATTEMPT_EXCEEDED");
}

/**
 * TC-0x27-008: SendKey (even sub-fn) without prior seed
 *              → ERR_SEC_SEED_UNAVAILABLE (NRC 0x24 requestSequenceError per ISO 14229-1).
 */
ZTEST(test_service_0x27, tc008_key_without_seed)
{
    setup();

    /* [P1-SEC FIX] key buffer = UDS_ALGO_KEY_LEN (4 bytes) */
    uint8_t key[UDS_ALGO_KEY_LEN] = { 0x11U, 0x22U, 0x33U, 0x44U };
    uds_msg_buf_t key_req  = make_key_req(0x02U, key, (uint8_t)UDS_ALGO_KEY_LEN);
    uds_msg_buf_t key_resp = {0};

    uds_status_t rc = uds_service_0x27_handler(&g_srv, &key_req, &key_resp);

    /* ISO 14229-1: key with no pending seed → ERR_SEC_SEED_UNAVAILABLE → NRC 0x24 */
    zassert_equal(rc, UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE,
                  "Key without prior seed must return SEC_SEED_UNAVAILABLE");
}

/**
 * TC-0x27-009: Request length == 1 (SID only) → ERR_INVALID_PARAM.
 */
ZTEST(test_service_0x27, tc009_length_1_rejected)
{
    setup();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x27U;
    req.length  = 1U;
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x27_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "Length 1 must fail");
}

/**
 * TC-0x27-010: Key submission with length 2 ([SID, evenSubFn] — no key bytes)
 *              → ERR_INVALID_PARAM (NRC 0x13).
 */
ZTEST(test_service_0x27, tc010_key_missing_key_bytes)
{
    setup();

    /* First request a seed so state is SEED_PENDING */
    uds_msg_buf_t seed_req  = make_seed_req(0x01U);
    uds_msg_buf_t seed_resp = {0};
    uds_service_0x27_handler(&g_srv, &seed_req, &seed_resp);

    /* Key request with no key bytes */
    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x27U;
    req.data[1] = 0x02U; /* even sub-fn = key path */
    req.length  = 2U;    /* no key bytes */
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x27_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "Key with no key bytes must fail with ERR_INVALID_PARAM");
}

/**
 * TC-0x27-011: Odd sub-function 0x03 (level-2 seed request) → 0x67 0x03.
 */
ZTEST(test_service_0x27, tc011_level2_seed_request)
{
    setup();

    uds_msg_buf_t req  = make_seed_req(0x03U); /* Level 2 seed */
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x27_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,   "Level 2 seed must succeed");
    zassert_equal(resp.data[0], 0x67U, "Response SID must be 0x67");
    zassert_equal(resp.data[1], 0x03U, "Sub-function echoed as 0x03");
}

/**
 * TC-0x27-012: Even sub-function 0x04 → processed as key path (not seed path).
 * Without a prior seed it must fail with ERR_SEC_SEED_UNAVAILABLE (NRC 0x24).
 */
ZTEST(test_service_0x27, tc012_even_subfunction_key_path)
{
    setup();

    /* [P1-SEC FIX] key buffer = UDS_ALGO_KEY_LEN (4 bytes) */
    uint8_t key[UDS_ALGO_KEY_LEN] = { 0xAAU, 0xBBU, 0xCCU, 0xDDU };
    uds_msg_buf_t req  = make_key_req(0x04U, key, (uint8_t)UDS_ALGO_KEY_LEN);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x27_handler(&g_srv, &req, &resp);

    /* No prior seed at level 0x03 → SEC_SEED_UNAVAILABLE (ISO 14229-1 compliant) */
    zassert_equal(rc, UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE,
                  "Even subFn 0x04 without seed at 0x03 must return SEC_SEED_UNAVAILABLE");
}

/**
 * TC-0x27-013: NULL req → ERR_NULL_PTR.
 */
ZTEST(test_service_0x27, tc013_null_req)
{
    setup();
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x27_handler(&g_srv, NULL, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL req must fail");
}

/**
 * TC-0x27-014: NULL resp → ERR_NULL_PTR from write_pos_sid.
 */
ZTEST(test_service_0x27, tc014_null_resp)
{
    setup();
    uds_msg_buf_t req = make_seed_req(0x01U);

    uds_status_t rc = uds_service_0x27_handler(&g_srv, &req, NULL);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL resp must fail");
}

/**
 * TC-0x27-015: After lockout expires (tick > lockout_ms), seed request succeeds.
 * Uses the server tick to drive both session and security timers.
 */
ZTEST(test_service_0x27, tc015_lockout_expiry_allows_retry)
{
    setup();

    /* [P1-SEC FIX] key buffer = UDS_ALGO_KEY_LEN (4 bytes) */
    uint8_t bad_key[UDS_ALGO_KEY_LEN] = { 0xFFU, 0xFFU, 0xFFU, 0xFFU };

    /* Trigger lockout */
    for (int i = 0; i < 4; i++) {
        uds_msg_buf_t sr  = make_seed_req(0x01U);
        uds_msg_buf_t srp = {0};
        uds_status_t  rc  = uds_service_0x27_handler(&g_srv, &sr, &srp);
        if (rc != UDS_STATUS_OK) break;

        uds_msg_buf_t kr  = make_key_req(0x02U, bad_key, (uint8_t)UDS_ALGO_KEY_LEN);
        uds_msg_buf_t krp = {0};
        uds_service_0x27_handler(&g_srv, &kr, &krp);

        if (g_sec.locked_out) break;
    }
    zassert_true(g_sec.locked_out, "Must be locked out");

    /* Tick through lockout period (lockout_ms = 10) */
    for (int t = 0; t <= 12; t++) {
        uds_server_tick_1ms(&g_srv);
    }
    zassert_false(g_sec.locked_out, "Lockout must have expired");

    /* Seed request must succeed */
    uds_msg_buf_t req  = make_seed_req(0x01U);
    uds_msg_buf_t resp = {0};
    uds_status_t  rc   = uds_service_0x27_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,
                  "Seed request must succeed after lockout expiry");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_service_0x27__tc001_seed_request_ok(void);
extern void test_service_0x27__tc002_seed_length(void);
extern void test_service_0x27__tc003_response_length(void);
extern void test_service_0x27__tc004_already_unlocked_zero_seed(void);
extern void test_service_0x27__tc005_correct_key_unlocks(void);
extern void test_service_0x27__tc006_wrong_key_rejected(void);
extern void test_service_0x27__tc007_lockout_after_max_attempts(void);
extern void test_service_0x27__tc008_key_without_seed(void);
extern void test_service_0x27__tc009_length_1_rejected(void);
extern void test_service_0x27__tc010_key_missing_key_bytes(void);
extern void test_service_0x27__tc011_level2_seed_request(void);
extern void test_service_0x27__tc012_even_subfunction_key_path(void);
extern void test_service_0x27__tc013_null_req(void);
extern void test_service_0x27__tc014_null_resp(void);
extern void test_service_0x27__tc015_lockout_expiry_allows_retry(void);

void run_all_tests(void)
{
    RUN_TEST(test_service_0x27__tc001_seed_request_ok);
    RUN_TEST(test_service_0x27__tc002_seed_length);
    RUN_TEST(test_service_0x27__tc003_response_length);
    RUN_TEST(test_service_0x27__tc004_already_unlocked_zero_seed);
    RUN_TEST(test_service_0x27__tc005_correct_key_unlocks);
    RUN_TEST(test_service_0x27__tc006_wrong_key_rejected);
    RUN_TEST(test_service_0x27__tc007_lockout_after_max_attempts);
    RUN_TEST(test_service_0x27__tc008_key_without_seed);
    RUN_TEST(test_service_0x27__tc009_length_1_rejected);
    RUN_TEST(test_service_0x27__tc010_key_missing_key_bytes);
    RUN_TEST(test_service_0x27__tc011_level2_seed_request);
    RUN_TEST(test_service_0x27__tc012_even_subfunction_key_path);
    RUN_TEST(test_service_0x27__tc013_null_req);
    RUN_TEST(test_service_0x27__tc014_null_resp);
    RUN_TEST(test_service_0x27__tc015_lockout_expiry_allows_retry);
}
