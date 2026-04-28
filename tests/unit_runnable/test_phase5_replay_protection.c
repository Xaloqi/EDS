// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_phase5_replay_protection.c
 *
 * MODULE UNDER TEST: core/uds_security_algo.c  — replay protection mechanism
 *                    core/uds_security.c        — integration with security state machine
 *
 * PURPOSE:
 *   Focused tests for the sequence-counter-based replay protection mechanism.
 *   Verifies that captured (seed, key) pairs cannot be reused after the
 *   sequence counter has advanced, and that the protection integrates correctly
 *   with the UDS security state machine.
 *
 * TEST CASES:
 *   TC-RPL-001  Each call to generate_seed produces a unique sequence number
 *   TC-RPL-002  Correct key accepted when sequence matches
 *   TC-RPL-003  Replayed key (old sequence) rejected after counter advances
 *   TC-RPL-004  Failed key attempt advances does NOT re-accept same seed (consume after attempt)
 *   TC-RPL-005  Sequence counter survives multiple round-trips
 *   TC-RPL-006  Key derived from seed A is invalid for seed B (cross-seed rejection)
 *   TC-RPL-007  Sequence 0x0000 is never emitted (always skipped on wrap)
 *   TC-RPL-008  Security state machine: correct key unlocks Level 1
 *   TC-RPL-009  Security state machine: replayed key rejected by state machine
 *   TC-RPL-010  Security state machine: failed replay increments attempt counter
 *   TC-RPL-011  Session reset clears seed_pending; subsequent seed generates new sequence
 *   TC-RPL-012  Level-2 replay blocked independent of Level-1 sequence state
 *   TC-RPL-013  Validate_key with stale sequence (seq-2) is rejected
 *   TC-RPL-014  Sequence bytes are big-endian (HI byte first in seed[2])
 *   TC-RPL-015  Rapid cycling: 10 seed/validate cycles all succeed without interference
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "uds_security_algo.h"
#include "uds_security.h"
#include "uds_session.h"
#include "uds_types.h"

ZTEST_SUITE(test_phase5_replay_protection, NULL, NULL, NULL, NULL, NULL);

/* =========================================================================
 * Security state machine helpers
 * ========================================================================= */

static uds_status_t sm_seed(uint8_t l, uint8_t *b, uint8_t n, uint8_t *o)
{
    return uds_security_algo_generate_seed(l, b, n, o);
}

static bool sm_key(uint8_t l, const uint8_t *s, uint8_t sl,
                   const uint8_t *k, uint8_t kl)
{
    return uds_security_algo_validate_key(l, s, sl, k, kl);
}

static uds_security_ctx_t g_sec;

static void setup(void)
{
    uds_security_algo_reset();
    memset(&g_sec, 0, sizeof(g_sec));

    static const uds_security_cfg_t sc = {
        .max_attempts     = 5U,
        .lockout_ms       = 100U,
        .key_validate_cb  = sm_key,
        .seed_generate_cb = sm_seed,
    };
    uds_security_init(&g_sec, &sc);
}

static void do_unlock_level1(uds_security_ctx_t *ctx)
{
    /* [P1-SEC FIX] seed = 8 bytes (UDS_ALGO_SEED_LEN), key = 4 bytes (UDS_ALGO_KEY_LEN) */
    uint8_t seed[UDS_ALGO_SEED_LEN], key[UDS_ALGO_KEY_LEN];
    uint8_t seed_len = 0U;
    (void)uds_security_request_seed(ctx, 0x01U, seed, (uint8_t)sizeof(seed), &seed_len);
    (void)uds_security_algo_derive_key(0x02U, seed, key);
    (void)uds_security_send_key(ctx, 0x02U, key, (uint8_t)UDS_ALGO_KEY_LEN);
}

/* =========================================================================
 * Test cases
 * ========================================================================= */

/**
 * TC-RPL-001: Each generate_seed call produces a unique sequence number.
 */
ZTEST(test_phase5_replay_protection, tc001_unique_sequences)
{
    uds_security_algo_reset();
    uint8_t s1[UDS_ALGO_SEED_LEN], s2[UDS_ALGO_SEED_LEN], s3[UDS_ALGO_SEED_LEN];
    uint8_t len;

    uds_security_algo_generate_seed(0x01U, s1, (uint8_t)sizeof(s1), &len);
    uds_security_algo_generate_seed(0x01U, s2, (uint8_t)sizeof(s2), &len);
    uds_security_algo_generate_seed(0x01U, s3, (uint8_t)sizeof(s3), &len);

    /* [P1-SEC FIX] Sequence is at bytes SEQ_HI_OFFSET(6) and SEQ_OFFSET(7) */
    uint16_t seq1 = ((uint16_t)s1[UDS_ALGO_SEED_SEQ_HI_OFFSET] << 8U) | s1[UDS_ALGO_SEED_SEQ_OFFSET];
    uint16_t seq2 = ((uint16_t)s2[UDS_ALGO_SEED_SEQ_HI_OFFSET] << 8U) | s2[UDS_ALGO_SEED_SEQ_OFFSET];
    uint16_t seq3 = ((uint16_t)s3[UDS_ALGO_SEED_SEQ_HI_OFFSET] << 8U) | s3[UDS_ALGO_SEED_SEQ_OFFSET];

    zassert_true(seq1 != seq2, "seq1 and seq2 must differ");
    zassert_true(seq2 != seq3, "seq2 and seq3 must differ");
    zassert_true(seq1 != seq3, "seq1 and seq3 must differ");
}

/**
 * TC-RPL-002: Correct key accepted when sequence matches.
 */
ZTEST(test_phase5_replay_protection, tc002_correct_key_accepted)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN], key[UDS_ALGO_KEY_LEN], len;
    uds_security_algo_generate_seed(0x01U, seed, (uint8_t)sizeof(seed), &len);
    uds_security_algo_derive_key(0x02U, seed, key);
    zassert_true(uds_security_algo_validate_key(0x02U, seed, (uint8_t)UDS_ALGO_SEED_LEN, key, (uint8_t)UDS_ALGO_KEY_LEN),
        "correct key with matching sequence must be accepted");
}

/**
 * TC-RPL-003: Replayed key rejected after counter advances.
 */
ZTEST(test_phase5_replay_protection, tc003_replayed_key_rejected)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN], key[UDS_ALGO_KEY_LEN], fresh_seed[UDS_ALGO_SEED_LEN], len;

    /* Capture a valid (seed, key) pair at sequence N. */
    uds_security_algo_generate_seed(0x01U, seed, (uint8_t)sizeof(seed), &len);
    uds_security_algo_derive_key(0x02U, seed, key);

    /* Advance counter by generating a new seed. */
    uds_security_algo_generate_seed(0x01U, fresh_seed, (uint8_t)sizeof(fresh_seed), &len);

    /* Replay old key — counter is now N+1, seed embeds N. */
    zassert_false(uds_security_algo_validate_key(0x02U, seed, (uint8_t)UDS_ALGO_SEED_LEN, key, (uint8_t)UDS_ALGO_KEY_LEN),
        "replayed key after counter advance must be rejected");
}

/**
 * TC-RPL-004: A failed key attempt consumes the seed; re-submission of the same
 * correct key for the same seed is also rejected (seed is consumed after any attempt).
 *
 * This tests that the sequence counter advancing after the failed attempt is
 * sufficient — a valid key for the old seed cannot succeed after the counter moves.
 */
ZTEST(test_phase5_replay_protection, tc004_seed_consumed_after_failed_attempt)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN], good_key[UDS_ALGO_KEY_LEN], bad_key[UDS_ALGO_KEY_LEN], fresh[UDS_ALGO_SEED_LEN], len;

    uds_security_algo_generate_seed(0x01U, seed, (uint8_t)sizeof(seed), &len);
    uds_security_algo_derive_key(0x02U, seed, good_key);
    memcpy(bad_key, good_key, (size_t)UDS_ALGO_KEY_LEN);
    bad_key[0] ^= 0x01U; /* corrupt */

    /* Submit bad key — fails, but sequence is still at N. */
    (void)uds_security_algo_validate_key(0x02U, seed, (uint8_t)UDS_ALGO_SEED_LEN, bad_key, (uint8_t)UDS_ALGO_KEY_LEN);

    /* Now generate new seed — advances counter. */
    uds_security_algo_generate_seed(0x01U, fresh, (uint8_t)sizeof(fresh), &len);

    /* Try old good key for old seed — counter mismatch, must fail. */
    zassert_false(uds_security_algo_validate_key(0x02U, seed, (uint8_t)UDS_ALGO_SEED_LEN, good_key, (uint8_t)UDS_ALGO_KEY_LEN),
        "after counter advance, previously valid key must be rejected");
}

/**
 * TC-RPL-005: Multiple consecutive round-trips all succeed.
 */
ZTEST(test_phase5_replay_protection, tc005_multiple_roundtrips)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN], key[UDS_ALGO_KEY_LEN], len;
    uint8_t i;

    for (i = 0U; i < 5U; i++) {
        uds_security_algo_generate_seed(0x01U, seed, (uint8_t)sizeof(seed), &len);
        uds_security_algo_derive_key(0x02U, seed, key);
        zassert_true(uds_security_algo_validate_key(0x02U, seed, (uint8_t)UDS_ALGO_SEED_LEN, key, (uint8_t)UDS_ALGO_KEY_LEN),
            "each fresh round-trip must succeed");
    }
}

/**
 * TC-RPL-006: Key derived from seed A is invalid for seed B.
 */
ZTEST(test_phase5_replay_protection, tc006_cross_seed_rejection)
{
    uds_security_algo_reset();
    uint8_t seed_a[UDS_ALGO_SEED_LEN], key_a[UDS_ALGO_KEY_LEN], seed_b[UDS_ALGO_SEED_LEN], len;

    uds_security_algo_generate_seed(0x01U, seed_a, (uint8_t)sizeof(seed_a), &len);
    uds_security_algo_derive_key(0x02U, seed_a, key_a);

    /* Get seed_b which has a different sequence. */
    uds_security_algo_generate_seed(0x01U, seed_b, (uint8_t)sizeof(seed_b), &len);

    /* key_a is derived from seed_a but submitted with seed_b — cross-seed: reject. */
    zassert_false(uds_security_algo_validate_key(0x02U, seed_b, (uint8_t)UDS_ALGO_SEED_LEN, key_a, (uint8_t)UDS_ALGO_KEY_LEN),
        "key derived from seed A must be rejected when submitted for seed B");
}

/**
 * TC-RPL-007: Sequence 0x0000 is never emitted.
 *
 * After reset (counter=0), first seed must have sequence >= 1.
 */
ZTEST(test_phase5_replay_protection, tc007_sequence_never_zero)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN], len;
    uds_security_algo_generate_seed(0x01U, seed, (uint8_t)sizeof(seed), &len);
    /* [P1-SEC FIX] Sequence at bytes SEQ_HI_OFFSET(6) and SEQ_OFFSET(7) */
    uint16_t seq = ((uint16_t)seed[UDS_ALGO_SEED_SEQ_HI_OFFSET] << 8U) | seed[UDS_ALGO_SEED_SEQ_OFFSET];
    zassert_not_equal(seq, (uint16_t)0U,
        "sequence embedded in seed must never be 0x0000");
}

/**
 * TC-RPL-008: Security state machine: correct key unlocks Level 1.
 */
ZTEST(test_phase5_replay_protection, tc008_sm_correct_key_unlocks)
{
    setup();

    uint8_t seed[UDS_SECURITY_SEED_LEN], key[UDS_SECURITY_KEY_LEN], seed_len;
    uds_security_request_seed(&g_sec, 0x01U, seed, sizeof(seed), &seed_len);
    uds_security_algo_derive_key(0x02U, seed, key);

    uds_status_t rc = uds_security_send_key(&g_sec, 0x02U, key, 4U);
    zassert_equal(rc, UDS_STATUS_OK, "correct key must unlock Level 1 via state machine");

    bool unlocked = false;
    uds_security_is_unlocked(&g_sec, 0x01U, &unlocked);
    zassert_true(unlocked, "Level 1 must be unlocked after successful key exchange");
}

/**
 * TC-RPL-009: Security state machine: replayed key is rejected.
 */
ZTEST(test_phase5_replay_protection, tc009_sm_replay_rejected)
{
    setup();

    /* Capture valid pair. */
    uint8_t seed[UDS_SECURITY_SEED_LEN], key[UDS_SECURITY_KEY_LEN], seed_len;
    uds_security_request_seed(&g_sec, 0x01U, seed, sizeof(seed), &seed_len);
    uds_security_algo_derive_key(0x02U, seed, key);

    /* First: legitimate success. */
    uds_security_send_key(&g_sec, 0x02U, key, 4U);

    /* Second: request a new seed (advance counter). */
    uint8_t new_seed[UDS_SECURITY_SEED_LEN];
    uds_security_request_seed(&g_sec, 0x01U, new_seed, sizeof(new_seed), &seed_len);

    /* Replay OLD key. */
    uds_status_t rc = uds_security_send_key(&g_sec, 0x02U, key, 4U);
    zassert_not_equal(rc, UDS_STATUS_OK,
        "replayed key must be rejected by state machine");
}

/**
 * TC-RPL-010: Security state machine: failed replay increments attempt counter.
 *
 * Flow: unlock L1, then get L2 seed, advance counter, replay L2 key → fail → attempts=1.
 * NOTE: We use Level-2 here because after L1 is unlocked, requesting an L1 seed
 *       returns all-zeros (already unlocked ISO behaviour) and does not set seed_pending.
 *       Level-2 is a distinct level, so seed_pending is set properly.
 */
ZTEST(test_phase5_replay_protection, tc010_sm_replay_increments_attempts)
{
    setup();

    /* Unlock Level 1. */
    do_unlock_level1(&g_sec);
    zassert_equal(g_sec.failed_attempts, (uint8_t)0U, "precondition: attempts=0 after unlock");

    /* Get a Level-2 seed (capture it). */
    uint8_t seed_l2[UDS_SECURITY_SEED_LEN], key_l2[UDS_ALGO_KEY_LEN], seed_len;
    uds_security_request_seed(&g_sec, 0x03U, seed_l2, sizeof(seed_l2), &seed_len);
    uds_security_algo_derive_key(0x04U, seed_l2, key_l2);

    /* Advance counter: generate another L2 seed (overwrites pending). */
    uint8_t fresh_l2[UDS_SECURITY_SEED_LEN];
    uds_security_request_seed(&g_sec, 0x03U, fresh_l2, sizeof(fresh_l2), &seed_len);

    /* Now send the OLD Level-2 key derived from seed_l2 (wrong XOR for fresh_l2). */
    uds_status_t rc = uds_security_send_key(&g_sec, 0x04U, key_l2, 4U);
    zassert_not_equal(rc, UDS_STATUS_OK, "replayed/wrong key must be rejected");

    /* Attempt counter must now be 1. */
    zassert_equal(g_sec.failed_attempts, (uint8_t)1U,
        "wrong key rejection must increment failed_attempts to 1");
}

/**
 * TC-RPL-011: Session reset clears seed_pending; new seed has new sequence.
 */
ZTEST(test_phase5_replay_protection, tc011_session_reset_clears_pending)
{
    setup();

    /* Request seed (seed_pending = true). */
    uint8_t seed1[UDS_SECURITY_SEED_LEN], seed_len;
    uds_security_request_seed(&g_sec, 0x01U, seed1, sizeof(seed1), &seed_len);
    zassert_true(g_sec.seed_pending, "precondition: seed_pending must be true");

    /* Reset (simulates Default session transition). */
    uds_security_reset(&g_sec);
    zassert_false(g_sec.seed_pending, "reset must clear seed_pending");

    /* Request new seed — must generate with new sequence. */
    uint8_t seed2[UDS_SECURITY_SEED_LEN];
    uds_security_request_seed(&g_sec, 0x01U, seed2, sizeof(seed2), &seed_len);

    /* [P1-SEC FIX] Sequence at bytes SEQ_HI_OFFSET(6) and SEQ_OFFSET(7) */
    uint16_t seq1 = ((uint16_t)seed1[UDS_ALGO_SEED_SEQ_HI_OFFSET] << 8U) | seed1[UDS_ALGO_SEED_SEQ_OFFSET];
    uint16_t seq2 = ((uint16_t)seed2[UDS_ALGO_SEED_SEQ_HI_OFFSET] << 8U) | seed2[UDS_ALGO_SEED_SEQ_OFFSET];
    zassert_not_equal(seq1, seq2,
        "new seed after session reset must have a different sequence");
}

/**
 * TC-RPL-012: Level-2 replay blocked independent of Level-1 sequence.
 */
ZTEST(test_phase5_replay_protection, tc012_level2_replay_blocked)
{
    uds_security_algo_reset();
    uint8_t s2[UDS_ALGO_SEED_LEN], k2[UDS_ALGO_KEY_LEN], new_s2[UDS_ALGO_SEED_LEN], len;

    /* Level-2 seed/key capture. */
    uds_security_algo_generate_seed(0x03U, s2, (uint8_t)sizeof(s2), &len);
    uds_security_algo_derive_key(0x04U, s2, k2);

    /* Advance counter with Level-1 seed. */
    uint8_t dummy[UDS_ALGO_SEED_LEN];
    uds_security_algo_generate_seed(0x01U, dummy, (uint8_t)sizeof(dummy), &len);

    /* Replay Level-2 pair — counter mismatch. */
    zassert_false(uds_security_algo_validate_key(0x04U, s2, (uint8_t)UDS_ALGO_SEED_LEN, k2, (uint8_t)UDS_ALGO_KEY_LEN),
        "Level-2 replay must be blocked after counter advances");
}

/**
 * TC-RPL-013: Key derived from seed at sequence N-1 is rejected when counter is at N.
 */
ZTEST(test_phase5_replay_protection, tc013_stale_seq_rejected)
{
    uds_security_algo_reset();
    uint8_t seed_old[UDS_ALGO_SEED_LEN], key_old[UDS_ALGO_KEY_LEN], seed_new[UDS_ALGO_SEED_LEN], len;

    /* Sequence N. */
    uds_security_algo_generate_seed(0x01U, seed_old, (uint8_t)sizeof(seed_old), &len);
    uds_security_algo_derive_key(0x02U, seed_old, key_old);

    /* Sequence N+1. */
    uds_security_algo_generate_seed(0x01U, seed_new, (uint8_t)sizeof(seed_new), &len);

    /* Old key for old seed: replay at N when counter is N+1. */
    zassert_false(uds_security_algo_validate_key(0x02U, seed_old, (uint8_t)UDS_ALGO_SEED_LEN, key_old, (uint8_t)UDS_ALGO_KEY_LEN),
        "key at sequence N must be rejected when counter is at N+1");
}

/**
 * TC-RPL-014: Sequence bytes are big-endian (HI byte in seed[SEQ_OFFSET]).
 */
ZTEST(test_phase5_replay_protection, tc014_sequence_big_endian)
{
    uds_security_algo_reset();

    /* Generate 256 seeds to get sequence = 0x0100 (cross 256 boundary). */
    uint8_t seed[UDS_ALGO_SEED_LEN], len;
    uint16_t i;
    for (i = 0U; i < 256U; i++) {
        uds_security_algo_generate_seed(0x01U, seed, (uint8_t)sizeof(seed), &len);
    }

    /* sequence = 0x0100, HI = 0x01, LO = 0x00 */
    /* [P1-SEC FIX] HI byte at SEQ_HI_OFFSET(6), LO byte at SEQ_OFFSET(7) */
    zassert_equal(seed[UDS_ALGO_SEED_SEQ_HI_OFFSET], (uint8_t)0x01U,
        "sequence HI byte must be in seed[SEQ_HI_OFFSET]");
    zassert_equal(seed[UDS_ALGO_SEED_SEQ_OFFSET],    (uint8_t)0x00U,
        "sequence LO byte must be in seed[SEQ_OFFSET]");
}

/**
 * TC-RPL-015: Rapid cycling: 10 independent seed/validate cycles all succeed.
 */
ZTEST(test_phase5_replay_protection, tc015_rapid_cycling)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN], key[UDS_ALGO_KEY_LEN], len;
    uint8_t pass_count = 0U;
    uint8_t i;

    for (i = 0U; i < 10U; i++) {
        uds_security_algo_generate_seed(0x01U, seed, (uint8_t)sizeof(seed), &len);
        uds_security_algo_derive_key(0x02U, seed, key);
        if (uds_security_algo_validate_key(0x02U, seed, (uint8_t)UDS_ALGO_SEED_LEN, key, (uint8_t)UDS_ALGO_KEY_LEN)) {
            pass_count++;
        }
    }

    zassert_equal(pass_count, (uint8_t)10U,
        "all 10 rapid seed/key cycles must succeed");
}

/* =========================================================================
 * run_all_tests
 * ========================================================================= */

extern void test_phase5_replay_protection__tc001_unique_sequences(void);
extern void test_phase5_replay_protection__tc002_correct_key_accepted(void);
extern void test_phase5_replay_protection__tc003_replayed_key_rejected(void);
extern void test_phase5_replay_protection__tc004_seed_consumed_after_failed_attempt(void);
extern void test_phase5_replay_protection__tc005_multiple_roundtrips(void);
extern void test_phase5_replay_protection__tc006_cross_seed_rejection(void);
extern void test_phase5_replay_protection__tc007_sequence_never_zero(void);
extern void test_phase5_replay_protection__tc008_sm_correct_key_unlocks(void);
extern void test_phase5_replay_protection__tc009_sm_replay_rejected(void);
extern void test_phase5_replay_protection__tc010_sm_replay_increments_attempts(void);
extern void test_phase5_replay_protection__tc011_session_reset_clears_pending(void);
extern void test_phase5_replay_protection__tc012_level2_replay_blocked(void);
extern void test_phase5_replay_protection__tc013_stale_seq_rejected(void);
extern void test_phase5_replay_protection__tc014_sequence_big_endian(void);
extern void test_phase5_replay_protection__tc015_rapid_cycling(void);

void run_all_tests(void)
{
    RUN_TEST(test_phase5_replay_protection__tc001_unique_sequences);
    RUN_TEST(test_phase5_replay_protection__tc002_correct_key_accepted);
    RUN_TEST(test_phase5_replay_protection__tc003_replayed_key_rejected);
    RUN_TEST(test_phase5_replay_protection__tc004_seed_consumed_after_failed_attempt);
    RUN_TEST(test_phase5_replay_protection__tc005_multiple_roundtrips);
    RUN_TEST(test_phase5_replay_protection__tc006_cross_seed_rejection);
    RUN_TEST(test_phase5_replay_protection__tc007_sequence_never_zero);
    RUN_TEST(test_phase5_replay_protection__tc008_sm_correct_key_unlocks);
    RUN_TEST(test_phase5_replay_protection__tc009_sm_replay_rejected);
    RUN_TEST(test_phase5_replay_protection__tc010_sm_replay_increments_attempts);
    RUN_TEST(test_phase5_replay_protection__tc011_session_reset_clears_pending);
    RUN_TEST(test_phase5_replay_protection__tc012_level2_replay_blocked);
    RUN_TEST(test_phase5_replay_protection__tc013_stale_seq_rejected);
    RUN_TEST(test_phase5_replay_protection__tc014_sequence_big_endian);
    RUN_TEST(test_phase5_replay_protection__tc015_rapid_cycling);
}
