// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/uds_sha256.c
 *
 * PURPOSE: Portable streaming SHA-256 (FIPS 180-4).
 *
 * PHASE 2 — DFU firmware authenticity, hardware-independent half [#232]
 *
 * See uds_sha256.h for full design documentation.
 *
 * IMPLEMENTATION NOTES:
 *
 *   Compression function (FIPS 180-4 section 6.2.2)
 *   -----------------------------------------------
 *   Each 64-byte block is expanded into a 64-word message schedule W[0..63]:
 *
 *       W[t]           = M[t]                            for  0 <= t <= 15
 *       W[t]           = s1(W[t-2]) + W[t-7]
 *                      + s0(W[t-15]) + W[t-16]           for 16 <= t <= 63
 *
 *   then mixed into the working variables a..h across 64 rounds using the
 *   Ch/Maj/Sigma0/Sigma1 functions and the 64 round constants K[t] (the
 *   first 32 bits of the fractional parts of the cube roots of the first 64
 *   primes).  The chaining value is updated by word-wise addition mod 2^32.
 *
 *   The message schedule is kept as a full 64-word array rather than the
 *   16-word sliding window some implementations use.  The 256-byte stack
 *   cost is affordable here, and the straight transcription of the standard
 *   is far easier to review against FIPS 180-4 than the rotated variant —
 *   which matters more than the RAM on an ASIL-B-oriented codebase.
 *
 *   Streaming (why this file has a buffer at all)
 *   ---------------------------------------------
 *   SHA-256 only consumes whole 64-byte blocks, but the caller feeds it
 *   whatever the tester's 0x36 block length happens to be.  update() keeps
 *   the sub-block remainder in ctx->buffer and compresses whole blocks
 *   directly out of the caller's data wherever it can, so a large chunk
 *   costs exactly one copy of its ragged head and tail, not a copy of the
 *   whole chunk.  ctx->total_len_bits accumulates independently of the
 *   buffering, so the padded length is correct no matter how the message
 *   was split.
 *
 *   Endianness
 *   ----------
 *   SHA-256 is defined on big-endian words.  Every word<->byte conversion
 *   in this file is an explicit shift-and-mask, so the object code is
 *   identical on a big- or little-endian target and no host header, union
 *   or pointer cast is involved.
 *
 * MISRA C:2012 DEVIATION LOG:
 *   [DEV-SHA-01] Rule 12.2 — every shift count in this file is a literal in
 *     the range 1..31 applied to a uint32_t operand, or 1..56 applied to a
 *     uint64_t operand.  No shift count can reach or exceed the width of
 *     its essential type, so the rule is satisfied by construction rather
 *     than by a run-time check.
 *   [DEV-SHA-02] Rule 10.3/10.4 — intermediate results of +, ^ and ~ on
 *     uint32_t operands are re-cast to uint32_t at every assignment so that
 *     the essential type is preserved through the integer promotions the
 *     standard mandates.  The wrap-around on + is intentional: SHA-256 is
 *     specified as addition modulo 2^32.
 *   [DEV-SHA-03] Rule 8.9 — the round constant table k_sha256_k and the
 *     initial hash value k_sha256_h0 are file-scope rather than
 *     block-scope.  They are shared by two functions and placing them in
 *     the smallest enclosing block would duplicate them in flash.
 *   [DEV-SHA-04] Rule 15.5 (single point of exit) — the three public
 *     functions return early on a NULL argument.  Deviation justified: the
 *     alternative is to wrap every body in a conditional block, which
 *     increases nesting without changing behaviour.  Same pattern as the
 *     guard clauses in platform/uds_image_policy.c.
 *
 * SAFETY  : Security-relevant. ASIL-B candidate. See uds_sha256.h.
 * STANDARD: FIPS PUB 180-4. MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "uds_sha256.h"

#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* --------------------------------------------------------------------------
 * Local constants
 * -------------------------------------------------------------------------- */

/** Number of compression rounds per block (FIPS 180-4 section 6.2.2). */
#define SHA256_ROUNDS       (64U)

/** Number of message-schedule words, one per round. */
#define SHA256_SCHED_WORDS  (64U)

/** Byte offset within a block at which the 64-bit length field starts. */
#define SHA256_LEN_OFFSET   (56U)

/**
 * Largest buffer_len — the 0x80 terminator ALREADY counted — at which the
 * 8-byte length field still fits in the current block.  The length field
 * occupies bytes [56, 64), so anything past byte 56 collides with it and
 * the padding must spill into a second, all-zero block.
 *
 * NOTE the off-by-one trap here: the largest MESSAGE tail that fits is 55
 * bytes, but the value compared against is buffer_len AFTER the 0x80 has
 * been appended, which is 56.  Getting this wrong produces a digest that is
 * correct for every length except those congruent to 55 mod 64 — a defect
 * no single-vector test would catch, which is why
 * tests/unit_runnable/test_sha256.c checks lengths 55/56/57/63/64/65/119
 * explicitly.
 */
#define SHA256_MAX_BUFFERED (SHA256_LEN_OFFSET)

/**
 * SHA-256 initial hash value H(0) — FIPS 180-4 section 5.3.3.
 * First 32 bits of the fractional parts of the square roots of the first
 * eight primes (2, 3, 5, 7, 11, 13, 17, 19).
 */
static const uint32_t k_sha256_h0[UDS_SHA256_STATE_WORDS] = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
};

/**
 * SHA-256 round constants K(0..63) — FIPS 180-4 section 4.2.2.
 * First 32 bits of the fractional parts of the cube roots of the first 64
 * primes (2 .. 311).
 */
static const uint32_t k_sha256_k[SHA256_ROUNDS] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

/* --------------------------------------------------------------------------
 * Word helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief Rotate a 32-bit word right by n bits.
 *
 * @param[in] x  Word to rotate.
 * @param[in] n  Rotate distance, 1..31.  Never 0 or 32 at any call site in
 *               this file, so the undefined `x >> 32` case cannot arise.
 * @return  x rotated right by n.
 */
static uint32_t sha256_rotr(uint32_t x, uint32_t n)
{
    /* [DEV-SHA-01] n is a literal 2..25 at every call site. */
    return (uint32_t)((uint32_t)(x >> n) | (uint32_t)(x << (32U - n)));
}

/** FIPS 180-4 section 4.1.2: Ch(x,y,z) = (x AND y) XOR (NOT x AND z). */
static uint32_t sha256_ch(uint32_t x, uint32_t y, uint32_t z)
{
    return (uint32_t)((uint32_t)(x & y) ^ (uint32_t)((uint32_t)(~x) & z));
}

/** FIPS 180-4 section 4.1.2: Maj(x,y,z) = (x AND y) XOR (x AND z) XOR (y AND z). */
static uint32_t sha256_maj(uint32_t x, uint32_t y, uint32_t z)
{
    return (uint32_t)((uint32_t)(x & y) ^ (uint32_t)(x & z) ^ (uint32_t)(y & z));
}

/** FIPS 180-4 section 4.1.2: BSIG0(x) = ROTR2(x) XOR ROTR13(x) XOR ROTR22(x). */
static uint32_t sha256_bsig0(uint32_t x)
{
    return (uint32_t)(sha256_rotr(x, 2U) ^ sha256_rotr(x, 13U) ^ sha256_rotr(x, 22U));
}

/** FIPS 180-4 section 4.1.2: BSIG1(x) = ROTR6(x) XOR ROTR11(x) XOR ROTR25(x). */
static uint32_t sha256_bsig1(uint32_t x)
{
    return (uint32_t)(sha256_rotr(x, 6U) ^ sha256_rotr(x, 11U) ^ sha256_rotr(x, 25U));
}

/** FIPS 180-4 section 4.1.2: SSIG0(x) = ROTR7(x) XOR ROTR18(x) XOR SHR3(x). */
static uint32_t sha256_ssig0(uint32_t x)
{
    return (uint32_t)(sha256_rotr(x, 7U) ^ sha256_rotr(x, 18U) ^ (uint32_t)(x >> 3U));
}

/** FIPS 180-4 section 4.1.2: SSIG1(x) = ROTR17(x) XOR ROTR19(x) XOR SHR10(x). */
static uint32_t sha256_ssig1(uint32_t x)
{
    return (uint32_t)(sha256_rotr(x, 17U) ^ sha256_rotr(x, 19U) ^ (uint32_t)(x >> 10U));
}

/**
 * @brief Assemble a big-endian 32-bit word from four bytes.
 *
 * Explicit byte assembly — never a pointer cast — so the result is
 * independent of host endianness and of the alignment of `b`.
 *
 * @param[in] b  Pointer to four bytes, most significant first.
 * @return  The assembled word.
 */
static uint32_t sha256_load_be32(const uint8_t *b)
{
    return (uint32_t)(((uint32_t)b[0U] << 24U) |
                      ((uint32_t)b[1U] << 16U) |
                      ((uint32_t)b[2U] <<  8U) |
                       (uint32_t)b[3U]);
}

/**
 * @brief Store a 32-bit word big-endian into four bytes.
 *
 * @param[in]  v  Word to store.
 * @param[out] b  Destination, four bytes, most significant first.
 */
static void sha256_store_be32(uint32_t v, uint8_t *b)
{
    b[0U] = (uint8_t)((v >> 24U) & 0xFFU);
    b[1U] = (uint8_t)((v >> 16U) & 0xFFU);
    b[2U] = (uint8_t)((v >>  8U) & 0xFFU);
    b[3U] = (uint8_t)( v         & 0xFFU);
}

/* --------------------------------------------------------------------------
 * Compression function
 * -------------------------------------------------------------------------- */

/**
 * @brief Compress one 64-byte block into the chaining state.
 *
 * Direct transcription of FIPS 180-4 section 6.2.2, steps 1 to 4.
 *
 * @param[in,out] state  Chaining value H0..H7, updated in place.
 * @param[in]     block  Exactly UDS_SHA256_BLOCK_LEN (64) message bytes.
 */
static void sha256_compress(uint32_t state[UDS_SHA256_STATE_WORDS],
                            const uint8_t block[UDS_SHA256_BLOCK_LEN])
{
    uint32_t w[SHA256_SCHED_WORDS];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    uint32_t t1;
    uint32_t t2;
    uint32_t t;

    /* Step 1: prepare the message schedule. */
    for (t = 0U; t < 16U; t++) {
        w[t] = sha256_load_be32(&block[t * 4U]);
    }
    for (t = 16U; t < SHA256_SCHED_WORDS; t++) {
        /* [DEV-SHA-02] addition is modulo 2^32 by specification. */
        w[t] = (uint32_t)(sha256_ssig1(w[t - 2U]) + w[t - 7U] +
                          sha256_ssig0(w[t - 15U]) + w[t - 16U]);
    }

    /* Step 2: initialise the working variables from the chaining value. */
    a = state[0U];
    b = state[1U];
    c = state[2U];
    d = state[3U];
    e = state[4U];
    f = state[5U];
    g = state[6U];
    h = state[7U];

    /* Step 3: 64 rounds. */
    for (t = 0U; t < SHA256_ROUNDS; t++) {
        t1 = (uint32_t)(h + sha256_bsig1(e) + sha256_ch(e, f, g) + k_sha256_k[t] + w[t]);
        t2 = (uint32_t)(sha256_bsig0(a) + sha256_maj(a, b, c));
        h  = g;
        g  = f;
        f  = e;
        e  = (uint32_t)(d + t1);
        d  = c;
        c  = b;
        b  = a;
        a  = (uint32_t)(t1 + t2);
    }

    /* Step 4: compute the intermediate hash value. */
    state[0U] = (uint32_t)(state[0U] + a);
    state[1U] = (uint32_t)(state[1U] + b);
    state[2U] = (uint32_t)(state[2U] + c);
    state[3U] = (uint32_t)(state[3U] + d);
    state[4U] = (uint32_t)(state[4U] + e);
    state[5U] = (uint32_t)(state[5U] + f);
    state[6U] = (uint32_t)(state[6U] + g);
    state[7U] = (uint32_t)(state[7U] + h);

    /* Scrub the schedule and working variables from the stack. */
    (void)memset(w, 0, sizeof(w));
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void uds_sha256_init(uds_sha256_ctx_t *ctx)
{
    uint32_t i;

    /* [DEV-SHA-04] guard clause. */
    if (ctx == NULL) {
        return;
    }

    for (i = 0U; i < UDS_SHA256_STATE_WORDS; i++) {
        ctx->state[i] = k_sha256_h0[i];
    }
    ctx->total_len_bits = (uint64_t)0U;
    ctx->buffer_len     = (size_t)0U;
    (void)memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

void uds_sha256_update(uds_sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t offset = (size_t)0U;
    size_t remaining;

    /* [DEV-SHA-04] guard clauses.  See the NULL-argument policy note in
     * uds_sha256.h: a skipped chunk can only ever produce a digest over too
     * few bytes, which fails the Phase 2 comparison closed. */
    if (ctx == NULL) {
        return;
    }
    if (len == (size_t)0U) {
        /* A zero-length update is a legitimate no-op; data may be NULL. */
        return;
    }
    if (data == NULL) {
        return;
    }

    /* The bit counter tracks the message, not the buffering, so it is
     * updated once here rather than per block. */
    ctx->total_len_bits += (uint64_t)((uint64_t)len * (uint64_t)8U);

    /* Phase 1: top up a partial block left over from a previous call. */
    if (ctx->buffer_len > (size_t)0U) {
        size_t space = (size_t)UDS_SHA256_BLOCK_LEN - ctx->buffer_len;
        size_t take  = (len < space) ? len : space;

        (void)memcpy(&ctx->buffer[ctx->buffer_len], &data[0U], take);
        ctx->buffer_len += take;
        offset           = take;

        if (ctx->buffer_len == (size_t)UDS_SHA256_BLOCK_LEN) {
            sha256_compress(ctx->state, ctx->buffer);
            ctx->buffer_len = (size_t)0U;
        }
    }

    /* Phase 2: compress whole blocks straight out of the caller's buffer.
     * Bounded: the loop variable strictly increases by BLOCK_LEN and the
     * guard is evaluated against the caller-supplied len. */
    while (((size_t)(len - offset)) >= (size_t)UDS_SHA256_BLOCK_LEN) {
        sha256_compress(ctx->state, &data[offset]);
        offset += (size_t)UDS_SHA256_BLOCK_LEN;
    }

    /* Phase 3: keep the ragged tail for the next call (or for final()). */
    remaining = (size_t)(len - offset);
    if (remaining > (size_t)0U) {
        /* remaining < UDS_SHA256_BLOCK_LEN by the loop condition above, and
         * buffer_len is 0 here (phase 1 either emptied it or consumed all of
         * len), so this copy cannot overrun ctx->buffer. */
        (void)memcpy(&ctx->buffer[ctx->buffer_len], &data[offset], remaining);
        ctx->buffer_len += remaining;
    }
}

void uds_sha256_final(uds_sha256_ctx_t *ctx, uint8_t digest[UDS_SHA256_DIGEST_LEN])
{
    uint64_t bit_len;
    size_t   i;
    uint32_t w;

    /* [DEV-SHA-04] guard clauses. */
    if (ctx == NULL) {
        return;
    }
    if (digest == NULL) {
        return;
    }

    bit_len = ctx->total_len_bits;

    /* FIPS 180-4 section 5.1.1: append 0x80, then the minimum number of
     * zero bytes such that the length field lands in the last 8 bytes of a
     * block. */
    ctx->buffer[ctx->buffer_len] = (uint8_t)0x80U;
    ctx->buffer_len += (size_t)1U;

    if (ctx->buffer_len > (size_t)SHA256_MAX_BUFFERED) {
        /* The length field does not fit: zero-fill and emit this block,
         * then pad a second, all-zero block. */
        (void)memset(&ctx->buffer[ctx->buffer_len], 0,
                     (size_t)UDS_SHA256_BLOCK_LEN - ctx->buffer_len);
        sha256_compress(ctx->state, ctx->buffer);
        ctx->buffer_len = (size_t)0U;
        (void)memset(ctx->buffer, 0, sizeof(ctx->buffer));
    } else {
        (void)memset(&ctx->buffer[ctx->buffer_len], 0,
                     (size_t)UDS_SHA256_BLOCK_LEN - ctx->buffer_len);
    }

    /* Append the 64-bit big-endian message length in bits. */
    for (i = (size_t)0U; i < (size_t)8U; i++) {
        uint32_t shift = (uint32_t)((uint32_t)56U - ((uint32_t)i * 8U));
        /* [DEV-SHA-01] shift is 56, 48, ... 0 — always below 64. */
        ctx->buffer[(size_t)SHA256_LEN_OFFSET + i] =
            (uint8_t)((bit_len >> shift) & (uint64_t)0xFFU);
    }

    sha256_compress(ctx->state, ctx->buffer);

    /* Emit H0..H7 big-endian. */
    for (i = (size_t)0U; i < (size_t)UDS_SHA256_STATE_WORDS; i++) {
        w = ctx->state[i];
        sha256_store_be32(w, &digest[i * (size_t)4U]);
    }

    /* Scrub the context.  The caller must re-init before any reuse. */
    (void)memset(ctx, 0, sizeof(*ctx));
}
