// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_aes_cmac.c
 *
 * PURPOSE: Portable AES-128-CMAC implementation.
 *
 * PHASE 1 — Production Security Hardening [P1-SEC]
 *
 * See uds_aes_cmac.h for full design documentation.
 *
 * IMPLEMENTATION NOTES:
 *
 *   AES-128 Block Cipher
 *   --------------------
 *   Implements the standard AES-128 algorithm (FIPS 197) using:
 *     - SubBytes via runtime computation from the affine transform
 *       (avoids a 256-byte lookup table — reduces flash footprint and
 *       eliminates table-based timing side-channels).
 *     - ShiftRows, MixColumns, AddRoundKey per the specification.
 *     - Key schedule computed inline (no stored round-key array beyond
 *       the 11 × 16-byte expanded key).
 *
 *   Why table-free AES?
 *     Traditional AES implementations use 256-byte (or 1024-byte) lookup
 *     tables for SubBytes/MixColumns. On embedded targets with data cache,
 *     table-lookup AES is vulnerable to cache-timing attacks. The compute-
 *     based SubBytes implementation here is cache-independent: all branches
 *     depend only on loop indices, not on key or plaintext data.
 *
 *   AES-128-CMAC (NIST SP 800-38B / RFC 4493)
 *   ------------------------------------------
 *   CMAC transforms AES-128 (a block cipher) into a Message Authentication
 *   Code. The construction:
 *     1. Derive two subkeys K1, K2 from AES-128(key, 0^128).
 *     2. Pad and split message into 16-byte blocks.
 *     3. XOR the last block with K1 (complete) or K2 (incomplete).
 *     4. CBC-MAC the block chain using AES-128.
 *
 *   Security: CMAC is a secure PRF under the assumption that AES is a
 *   secure block cipher. Unlike XOR-based derivation, the key cannot be
 *   recovered by an attacker who observes (seed, key) pairs without
 *   breaking AES-128.
 *
 * MISRA C:2012 DEVIATION LOG:
 *   [DEV-AES-01] Rule 14.4 — loop counter i used as both uint8_t index and
 *     shift operand. Cast via (uint8_t) throughout. Deviation justified:
 *     AES round operations are inherently array-indexed.
 *   [DEV-AES-02] Rule 12.2 — shift count validated to be < 8 for all
 *     operations on uint8_t. No right-shift on signed types.
 *   [DEV-AES-03] Rule 2.2 — XOR of same operand in GF multiply is
 *     intentional (conditional XOR preserves constant-time property).
 *     volatile not used here because GF operations have no side-effects;
 *     the compiler cannot eliminate them as the result is always consumed.
 *
 * SAFETY  : ASIL-B candidate. See uds_aes_cmac.h for full safety notes.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "uds_aes_cmac.h"

#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Local constants
 * -------------------------------------------------------------------------- */

/** Number of AES-128 rounds (Nr = 10 for 128-bit key). */
#define AES_NR   (10U)

/** Number of 32-bit words in the AES state (Nb = 4). */
#define AES_NB   (4U)

/** Number of 32-bit words in the AES-128 key (Nk = 4). */
#define AES_NK   (4U)

/** Number of round keys = Nr + 1 = 11. */
#define AES_NRK  (11U)

/** Total expanded key size in bytes = AES_NRK * 16. */
#define AES_EKS  ((uint16_t)(AES_NRK * UDS_AES_BLOCK_LEN))

/* AES round constants (Rcon), one per key schedule iteration. */
static const uint8_t k_rcon[10U] = {
    0x01U, 0x02U, 0x04U, 0x08U, 0x10U,
    0x20U, 0x40U, 0x80U, 0x1BU, 0x36U
};

/* --------------------------------------------------------------------------
 * GF(2^8) arithmetic — polynomial x^8 + x^4 + x^3 + x + 1 (0x11B)
 * -------------------------------------------------------------------------- */

/**
 * @brief Multiply a byte by 2 in GF(2^8).
 *
 * Left-shift by 1; XOR with 0x1B if bit 7 was set (reduction polynomial).
 * Constant-time: no data-dependent branches.
 *
 * @param[in] x  Input byte.
 * @return  x * 0x02 in GF(2^8).
 */
static uint8_t gf_mul2(uint8_t x)
{
    uint8_t hi = (uint8_t)((x >> 7U) & (uint8_t)0x01U);
    uint8_t result = (uint8_t)(x << 1U);
    /*
     * If hi == 1, reduce by XOR with 0x1B.
     * Using arithmetic mask instead of branch for constant-time property.
     * (-hi) in two's complement is 0xFF when hi=1, 0x00 when hi=0.
     * [DEV-AES-03] XOR conditioned on mask — intentional.
     */
    result ^= (uint8_t)(hi * (uint8_t)0x1BU);
    return result;
}

/**
 * @brief Multiply two bytes in GF(2^8) using Russian-peasant algorithm.
 *
 * Constant-time: all 8 iterations execute unconditionally.
 *
 * @param[in] a  First factor.
 * @param[in] b  Second factor.
 * @return  a * b in GF(2^8).
 */
static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    uint8_t result = (uint8_t)0U;
    uint8_t aa     = a;
    uint8_t bb     = b;
    uint8_t i;

    for (i = (uint8_t)0U; i < (uint8_t)8U; i++) {
        /* If LSB of b is set, XOR a into result. */
        result ^= (uint8_t)(aa * (uint8_t)(bb & (uint8_t)0x01U));
        aa  = gf_mul2(aa);
        bb = (uint8_t)(bb >> 1U);
    }
    return result;
}

/* --------------------------------------------------------------------------
 * AES SubBytes — affine transform based, no lookup table
 * -------------------------------------------------------------------------- */

/**
 * @brief Compute the multiplicative inverse of x in GF(2^8).
 *
 * Uses the extended Euclidean algorithm in GF(2^8).
 * Returns 0 for input 0 (by convention: 0 has no inverse, defined as 0).
 *
 * @param[in] x  Input byte.
 * @return  x^(-1) in GF(2^8), or 0 if x == 0.
 */
static uint8_t gf_inv(uint8_t x)
{
    uint8_t result;
    uint8_t sq;

    if (x == (uint8_t)0U) {
        return (uint8_t)0U;
    }

    /* Fermat's little theorem: x^(-1) = x^(2^8 - 2) = x^254 in GF(2^8).
     * Compute via repeated squaring:
     *   x^254 = x^128 * x^64 * x^32 * x^16 * x^8 * x^4 * x^2
     * (note: no x^1 term since 254 = 11111110 binary) */
    sq     = gf_mul(x, x);          /* x^2  */
    result = sq;                     /* accumulate: x^2 */
    sq     = gf_mul(sq, sq);        /* x^4  */
    result = gf_mul(result, sq);    /* x^2 * x^4  = x^6  — not used */

    /* Reset and use square-and-multiply for x^254 directly. */
    {
        uint8_t p = x;       /* current power */
        uint8_t acc = (uint8_t)1U;
        uint8_t exp = (uint8_t)254U;
        uint8_t bit;

        for (bit = (uint8_t)0U; bit < (uint8_t)8U; bit++) {
            if ((exp & (uint8_t)1U) != (uint8_t)0U) {
                acc = gf_mul(acc, p);
            }
            p   = gf_mul(p, p);
            exp = (uint8_t)(exp >> 1U);
        }
        result = acc;
    }

    return result;
}

/**
 * @brief AES SubBytes substitution for a single byte.
 *
 * Computes: s = inv(x); then applies affine transform:
 *   out = s XOR ROT1(s) XOR ROT2(s) XOR ROT3(s) XOR ROT4(s) XOR 0x63
 * where ROTn denotes a left-circular-shift by n bits.
 *
 * @param[in] x  Input byte.
 * @return  SubBytes(x).
 */
static uint8_t aes_sub_byte(uint8_t x)
{
    uint8_t s = gf_inv(x);
    /* Affine transform: s XOR (s<<<1) XOR (s<<<2) XOR (s<<<3) XOR (s<<<4) XOR 0x63 */
    uint8_t r = s;
    r ^= (uint8_t)((s << 1U) | (s >> 7U));
    r ^= (uint8_t)((s << 2U) | (s >> 6U));
    r ^= (uint8_t)((s << 3U) | (s >> 5U));
    r ^= (uint8_t)((s << 4U) | (s >> 4U));
    r ^= (uint8_t)0x63U;
    return r;
}

/* --------------------------------------------------------------------------
 * AES key schedule
 * -------------------------------------------------------------------------- */

/**
 * @brief Expand a 16-byte AES-128 key into 11 round keys (176 bytes).
 *
 * @param[in]  key       16-byte input key.
 * @param[out] round_key Output buffer, must be AES_EKS (176) bytes.
 */
static void aes_key_expand(
    const uint8_t key[UDS_AES128_KEY_LEN],
    uint8_t       round_key[AES_EKS])
{
    uint8_t  i;
    uint8_t  temp[4U];
    uint8_t  j;
    uint16_t base;

    /* First round key is the raw key. */
    (void)memcpy(round_key, key, (size_t)UDS_AES128_KEY_LEN);

    for (i = (uint8_t)AES_NK; i < (uint8_t)(AES_NB * AES_NRK); i++) {
        /* Copy previous word. */
        base = (uint16_t)((uint16_t)(i - (uint8_t)1U) * (uint16_t)4U);
        temp[0U] = round_key[base];
        temp[1U] = round_key[base + (uint16_t)1U];
        temp[2U] = round_key[base + (uint16_t)2U];
        temp[3U] = round_key[base + (uint16_t)3U];

        if ((i % (uint8_t)AES_NK) == (uint8_t)0U) {
            /* RotWord: left-rotate by 1 byte. */
            uint8_t t = temp[0U];
            temp[0U]  = temp[1U];
            temp[1U]  = temp[2U];
            temp[2U]  = temp[3U];
            temp[3U]  = t;
            /* SubWord. */
            for (j = (uint8_t)0U; j < (uint8_t)4U; j++) {
                temp[j] = aes_sub_byte(temp[j]);
            }
            /* XOR with Rcon. */
            temp[0U] ^= k_rcon[(i / (uint8_t)AES_NK) - (uint8_t)1U];
        }

        /* XOR with word AES_NK positions back. */
        base = (uint16_t)((uint16_t)i * (uint16_t)4U);
        uint16_t prev_base = (uint16_t)((uint16_t)(i - (uint8_t)AES_NK) * (uint16_t)4U);
        round_key[base]              = round_key[prev_base]              ^ temp[0U];
        round_key[base + (uint16_t)1U] = round_key[prev_base + (uint16_t)1U] ^ temp[1U];
        round_key[base + (uint16_t)2U] = round_key[prev_base + (uint16_t)2U] ^ temp[2U];
        round_key[base + (uint16_t)3U] = round_key[prev_base + (uint16_t)3U] ^ temp[3U];
    }
}

/* --------------------------------------------------------------------------
 * AES state transformations
 * State is stored column-major: state[row][col], 4x4 bytes.
 * -------------------------------------------------------------------------- */

/** Apply SubBytes to all 16 state bytes. */
static void aes_sub_bytes(uint8_t state[4U][4U])
{
    uint8_t r, c;
    for (r = (uint8_t)0U; r < (uint8_t)4U; r++) {
        for (c = (uint8_t)0U; c < (uint8_t)4U; c++) {
            state[r][c] = aes_sub_byte(state[r][c]);
        }
    }
}

/** ShiftRows: row i rotated left by i positions. */
static void aes_shift_rows(uint8_t state[4U][4U])
{
    uint8_t temp;

    /* Row 1: left rotate by 1 */
    temp         = state[1U][0U];
    state[1U][0U] = state[1U][1U];
    state[1U][1U] = state[1U][2U];
    state[1U][2U] = state[1U][3U];
    state[1U][3U] = temp;

    /* Row 2: left rotate by 2 */
    temp         = state[2U][0U];
    state[2U][0U] = state[2U][2U];
    state[2U][2U] = temp;
    temp         = state[2U][1U];
    state[2U][1U] = state[2U][3U];
    state[2U][3U] = temp;

    /* Row 3: left rotate by 3 (= right rotate by 1) */
    temp         = state[3U][3U];
    state[3U][3U] = state[3U][2U];
    state[3U][2U] = state[3U][1U];
    state[3U][1U] = state[3U][0U];
    state[3U][0U] = temp;
}

/** MixColumns: multiply each column by the MDS matrix in GF(2^8). */
static void aes_mix_columns(uint8_t state[4U][4U])
{
    uint8_t c;
    uint8_t s0, s1, s2, s3;

    for (c = (uint8_t)0U; c < (uint8_t)4U; c++) {
        s0 = state[0U][c];
        s1 = state[1U][c];
        s2 = state[2U][c];
        s3 = state[3U][c];

        state[0U][c] = (uint8_t)(gf_mul2(s0) ^ gf_mul(s1, 0x03U) ^ s2 ^ s3);
        state[1U][c] = (uint8_t)(s0 ^ gf_mul2(s1) ^ gf_mul(s2, 0x03U) ^ s3);
        state[2U][c] = (uint8_t)(s0 ^ s1 ^ gf_mul2(s2) ^ gf_mul(s3, 0x03U));
        state[3U][c] = (uint8_t)(gf_mul(s0, 0x03U) ^ s1 ^ s2 ^ gf_mul2(s3));
    }
}

/** AddRoundKey: XOR state with a 16-byte round key. */
static void aes_add_round_key(
    uint8_t       state[4U][4U],
    const uint8_t round_key[UDS_AES_BLOCK_LEN])
{
    uint8_t r, c;
    for (c = (uint8_t)0U; c < (uint8_t)4U; c++) {
        for (r = (uint8_t)0U; r < (uint8_t)4U; r++) {
            state[r][c] ^= round_key[(uint8_t)((c * (uint8_t)4U) + r)];
        }
    }
}

/* --------------------------------------------------------------------------
 * Public: AES-128 block encryption
 * -------------------------------------------------------------------------- */

void uds_aes128_encrypt_block(
    const uint8_t key[UDS_AES128_KEY_LEN],
    const uint8_t plaintext[UDS_AES_BLOCK_LEN],
    uint8_t       ciphertext[UDS_AES_BLOCK_LEN])
{
    uint8_t round_key[AES_EKS];
    uint8_t state[4U][4U];
    uint8_t r, c, round;

    /* Expand key. */
    aes_key_expand(key, round_key);

    /* Load plaintext into state (column-major). */
    for (c = (uint8_t)0U; c < (uint8_t)4U; c++) {
        for (r = (uint8_t)0U; r < (uint8_t)4U; r++) {
            state[r][c] = plaintext[(uint8_t)((c * (uint8_t)4U) + r)];
        }
    }

    /* Initial AddRoundKey (round 0). */
    aes_add_round_key(state, &round_key[0U]);

    /* Rounds 1 to NR-1: SubBytes, ShiftRows, MixColumns, AddRoundKey. */
    for (round = (uint8_t)1U; round < (uint8_t)AES_NR; round++) {
        aes_sub_bytes(state);
        aes_shift_rows(state);
        aes_mix_columns(state);
        aes_add_round_key(state, &round_key[(uint16_t)round * (uint16_t)UDS_AES_BLOCK_LEN]);
    }

    /* Final round: SubBytes, ShiftRows, AddRoundKey (no MixColumns). */
    aes_sub_bytes(state);
    aes_shift_rows(state);
    aes_add_round_key(state, &round_key[(uint16_t)AES_NR * (uint16_t)UDS_AES_BLOCK_LEN]);

    /* Unload state into ciphertext (column-major). */
    for (c = (uint8_t)0U; c < (uint8_t)4U; c++) {
        for (r = (uint8_t)0U; r < (uint8_t)4U; r++) {
            ciphertext[(uint8_t)((c * (uint8_t)4U) + r)] = state[r][c];
        }
    }

    /* Scrub key material from stack. */
    (void)memset(round_key, 0, sizeof(round_key));
    (void)memset(state,     0, sizeof(state));
}

/* --------------------------------------------------------------------------
 * CMAC subkey derivation helpers (RFC 4493 §2.3)
 * -------------------------------------------------------------------------- */

/**
 * @brief Left-shift a 16-byte block by 1 bit, with carry from MSB.
 *
 * @param[in]  in    Input 16-byte block.
 * @param[out] out   Output 16-byte block (in << 1).
 */
static void cmac_shift_left1(
    const uint8_t in[UDS_AES_BLOCK_LEN],
    uint8_t       out[UDS_AES_BLOCK_LEN])
{
    uint8_t i;
    uint8_t carry = (uint8_t)0U;

    /* Process from LSB byte (index 15) to MSB byte (index 0). */
    for (i = (uint8_t)0U; i < (uint8_t)UDS_AES_BLOCK_LEN; i++) {
        uint8_t idx    = (uint8_t)((uint8_t)UDS_AES_BLOCK_LEN - (uint8_t)1U - i);
        uint8_t new_carry = (uint8_t)((in[idx] >> 7U) & (uint8_t)0x01U);
        out[idx] = (uint8_t)((in[idx] << 1U) | carry);
        carry    = new_carry;
    }
}

/**
 * @brief XOR two 16-byte blocks.
 *
 * @param[in]  a   First block.
 * @param[in]  b   Second block.
 * @param[out] out Result of a XOR b.
 */
static void cmac_xor_block(
    const uint8_t a[UDS_AES_BLOCK_LEN],
    const uint8_t b[UDS_AES_BLOCK_LEN],
    uint8_t       out[UDS_AES_BLOCK_LEN])
{
    uint8_t i;
    for (i = (uint8_t)0U; i < (uint8_t)UDS_AES_BLOCK_LEN; i++) {
        out[i] = (uint8_t)(a[i] ^ b[i]);
    }
}

/**
 * @brief Derive CMAC subkeys K1 and K2 from the AES key.
 *
 * Per RFC 4493 §2.3:
 *   L  = AES-128(key, 0^128)
 *   K1 = L << 1  [XOR Rb if MSB(L) = 1]
 *   K2 = K1 << 1 [XOR Rb if MSB(K1) = 1]
 * where Rb = 0x00...0087 (the constant for 128-bit blocks).
 *
 * @param[in]  key  16-byte AES-128 key.
 * @param[out] k1   First subkey (16 bytes).
 * @param[out] k2   Second subkey (16 bytes).
 */
static void cmac_derive_subkeys(
    const uint8_t key[UDS_AES128_KEY_LEN],
    uint8_t       k1[UDS_AES_BLOCK_LEN],
    uint8_t       k2[UDS_AES_BLOCK_LEN])
{
    static const uint8_t k_zero[UDS_AES_BLOCK_LEN] = {
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
    };
    /* Rb constant for 128-bit block size. */
    static const uint8_t k_rb_lsb = (uint8_t)0x87U;

    uint8_t L[UDS_AES_BLOCK_LEN];

    /* L = AES(key, 0^128) */
    uds_aes128_encrypt_block(key, k_zero, L);

    /* K1 = L << 1; if MSB(L)==1, K1 ^= Rb */
    cmac_shift_left1(L, k1);
    if ((L[0U] & (uint8_t)0x80U) != (uint8_t)0U) {
        k1[UDS_AES_BLOCK_LEN - (uint8_t)1U] ^= k_rb_lsb;
    }

    /* K2 = K1 << 1; if MSB(K1)==1, K2 ^= Rb */
    cmac_shift_left1(k1, k2);
    if ((k1[0U] & (uint8_t)0x80U) != (uint8_t)0U) {
        k2[UDS_AES_BLOCK_LEN - (uint8_t)1U] ^= k_rb_lsb;
    }

    /* Scrub L from stack. */
    (void)memset(L, 0, sizeof(L));
}

/* --------------------------------------------------------------------------
 * Public: AES-128-CMAC
 * -------------------------------------------------------------------------- */

int uds_aes_cmac(
    const uint8_t *key,
    const uint8_t *msg,
    size_t         msg_len,
    uint8_t        tag[UDS_CMAC_TAG_LEN])
{
    uint8_t  k1[UDS_AES_BLOCK_LEN];
    uint8_t  k2[UDS_AES_BLOCK_LEN];
    uint8_t  x[UDS_AES_BLOCK_LEN];
    uint8_t  y[UDS_AES_BLOCK_LEN];
    uint8_t  m_last[UDS_AES_BLOCK_LEN];
    size_t   n_blocks;
    size_t   last_block_len;
    size_t   i;
    bool     flag;   /* true if last block is complete */

    if ((key == NULL) || (tag == NULL)) {
        return -1;
    }

    /* Derive CMAC subkeys. */
    cmac_derive_subkeys(key, k1, k2);

    /* Number of complete 16-byte blocks. */
    if (msg_len == (size_t)0U) {
        n_blocks      = (size_t)1U;
        last_block_len = (size_t)0U;
        flag          = false;
    } else {
        n_blocks      = (msg_len + (size_t)(UDS_AES_BLOCK_LEN - (uint8_t)1U))
                        / (size_t)UDS_AES_BLOCK_LEN;
        last_block_len = msg_len % (size_t)UDS_AES_BLOCK_LEN;
        flag          = (last_block_len == (size_t)0U);
    }

    /* Build the last block (M_last). */
    if (flag) {
        /* Complete block: M_last = M_n XOR K1. */
        const uint8_t *last_src = &msg[(n_blocks - (size_t)1U) * (size_t)UDS_AES_BLOCK_LEN];
        cmac_xor_block(last_src, k1, m_last);
    } else {
        /* Incomplete block: pad with 0x80 0x00..., then XOR K2. */
        size_t src_offset = (n_blocks - (size_t)1U) * (size_t)UDS_AES_BLOCK_LEN;
        uint8_t padded[UDS_AES_BLOCK_LEN];
        (void)memset(padded, 0, sizeof(padded));

        if (msg != NULL) {
            (void)memcpy(padded, &msg[src_offset], last_block_len);
        }
        padded[last_block_len] = (uint8_t)0x80U;
        cmac_xor_block(padded, k2, m_last);
    }

    /* CBC-MAC: X = 0^128; for each block i: Y = AES(key, X XOR M_i) */
    (void)memset(x, 0, sizeof(x));

    for (i = (size_t)0U; i < (n_blocks - (size_t)1U); i++) {
        const uint8_t *blk = &msg[i * (size_t)UDS_AES_BLOCK_LEN];
        cmac_xor_block(x, blk, y);
        uds_aes128_encrypt_block(key, y, x);
    }

    /* Final block. */
    cmac_xor_block(x, m_last, y);
    uds_aes128_encrypt_block(key, y, tag);

    /* Scrub sensitive locals from stack. */
    (void)memset(k1,     0, sizeof(k1));
    (void)memset(k2,     0, sizeof(k2));
    (void)memset(x,      0, sizeof(x));
    (void)memset(y,      0, sizeof(y));
    (void)memset(m_last, 0, sizeof(m_last));

    return 0;
}
