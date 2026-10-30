// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_service_0x36_block_verify.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x36.c, service_0x37.c
 *                    Per-block readback during TransferData [EDS#543]
 *
 * EDS#543: 0x37 read the whole download region back to check its CRC-32,
 * 73 ms for 512 KiB on the STM32H753, past P2server_max = 50 ms. With the
 * flash ops' verify_step_cb, each block is read back right after it is
 * written during 0x36, and 0x37 only compares CRC-32s it already holds.
 * The end-to-end guarantee stays: a block that reads back wrong is refused,
 * and the read-back region's CRC-32 must equal the received data's.
 *
 * Each test runs a real 0x34 -> 0x36 ... -> 0x37 download into a RAM-backed
 * flash whose write callback can be told to corrupt one write (fault
 * injection: the driver reports success but stores a flipped bit).
 *
 * Coverage:
 *   TC-0x36-BV-001  Clean download: every block read back, 0x37 accepts with
 *                   no whole-region verify_cb call
 *   TC-0x36-BV-002  Corrupted write of block 5 -> NRC 0x72 at that 0x36, the
 *                   transfer is aborted, and 0x37 cannot accept it
 *   TC-0x36-BV-003  Corrupted last (partial) chunk, flushed by 0x37 -> 0x72
 *   TC-0x36-BV-004  Readback driver failure -> NRC 0x72 at the block
 *   TC-0x36-BV-005  verify_step_cb returns OK without a CRC -> refused (fail closed)
 *   TC-0x36-BV-006  Read-back region CRC-32 differs from the received data
 *                   at 0x37 -> NRC 0x72 (the end-to-end check is live)
 *   TC-0x36-BV-007  Tester CRC-32 wrong -> NRC 0x72 (unchanged)
 *   TC-0x36-BV-008  No verify_step_cb -> whole-region verify_cb at 0x37
 *                   (and a corrupted block is still refused there)
 *   TC-0x36-BV-009  Flash ops without verify_step_cb swapped in mid-transfer
 *                   -> region not fully read back -> verify_cb at 0x37
 *   TC-0x36-BV-010  A repeated block (lost response) is not written or read
 *                   back twice
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h for host compilation)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_safety.h"
#include "uds_transfer_ctx.h"
#include "uds_flash_ops.h"

/* ==========================================================================
 * RAM-backed mock flash with write fault injection
 * ========================================================================== */

#define MOCK_FLASH_BASE   (0x08020000UL)
#define MOCK_FLASH_SIZE   (4096U)
#define MOCK_BLOCK_LEN    (256U)
#define NO_FAULT          (0xFFFFFFFFUL)

static uint8_t  s_flash[MOCK_FLASH_SIZE];
static uint32_t s_corrupt_write_no;     /* 1-based write_cb call to corrupt */
static bool     s_step_read_fail;
static bool     s_step_skip_crc_out;
static uint32_t s_write_calls;
static uint32_t s_step_calls;
static uint32_t s_step_bytes;
static uint32_t s_verify_calls;

static uds_status_t mock_erase(uint32_t address, uint32_t size_bytes)
{
    (void)memset(&s_flash[address - MOCK_FLASH_BASE], 0xFF, (size_t)size_bytes);
    return UDS_STATUS_OK;
}

static uds_status_t mock_write(uint32_t address, const uint8_t *data, uint32_t length)
{
    uint32_t off = address - MOCK_FLASH_BASE;

    s_write_calls++;
    (void)memcpy(&s_flash[off], data, (size_t)length);
    if (s_write_calls == s_corrupt_write_no) {
        s_flash[off + (length / 2U)] ^= 0x10U;   /* stored wrong, reported OK */
    }
    return UDS_STATUS_OK;
}

static uds_status_t mock_verify(uint32_t address, uint32_t size_bytes, uint32_t expected_crc)
{
    uint32_t crc;

    s_verify_calls++;
    crc = uds_transfer_crc32_update(0xFFFFFFFFUL, &s_flash[address - MOCK_FLASH_BASE], size_bytes);
    return (uds_transfer_crc32_finalise(crc) == expected_crc) ? UDS_STATUS_OK
                                                              : UDS_STATUS_ERR_GENERIC;
}

static uds_status_t mock_verify_step(uint32_t address, uint32_t length,
                                     uint32_t crc_in, uint32_t *crc_out)
{
    s_step_calls++;
    s_step_bytes += length;
    if (s_step_read_fail) {
        return UDS_STATUS_ERR_PLATFORM;
    }
    if (!s_step_skip_crc_out) {
        *crc_out = uds_transfer_crc32_update(crc_in, &s_flash[address - MOCK_FLASH_BASE], length);
    }
    return UDS_STATUS_OK;
}

static const uds_flash_region_t k_region[1U] = {
    { .base_address = MOCK_FLASH_BASE, .size_bytes = MOCK_FLASH_SIZE, .writable = true }
};

static const uds_flash_ops_t k_ops_step = {
    .erase_cb         = mock_erase,
    .write_cb         = mock_write,
    .verify_cb        = mock_verify,
    .verify_step_cb   = mock_verify_step,
    .memory_map       = k_region,
    .region_count     = (uint8_t)1U,
    .max_block_length = (uint16_t)MOCK_BLOCK_LEN,
};

static const uds_flash_ops_t k_ops_no_step = {
    .erase_cb         = mock_erase,
    .write_cb         = mock_write,
    .verify_cb        = mock_verify,
    .memory_map       = k_region,
    .region_count     = (uint8_t)1U,
    .max_block_length = (uint16_t)MOCK_BLOCK_LEN,
};

/* ==========================================================================
 * Test state and helpers
 * ========================================================================== */

static uds_session_ctx_t  s_sess;
static uds_security_ctx_t s_sec;
static uds_server_ctx_t   s_srv;
static uds_msg_buf_t      s_req;
static uds_msg_buf_t      s_resp;
static uint8_t            s_image[MOCK_FLASH_SIZE];

static uds_status_t send_0x34(uint32_t size)
{
    s_req.data[0]  = 0x34U;
    s_req.data[1]  = 0x00U;
    s_req.data[2]  = 0x44U;
    s_req.data[3]  = (uint8_t)(MOCK_FLASH_BASE >> 24U);
    s_req.data[4]  = (uint8_t)(MOCK_FLASH_BASE >> 16U);
    s_req.data[5]  = (uint8_t)(MOCK_FLASH_BASE >> 8U);
    s_req.data[6]  = (uint8_t)(MOCK_FLASH_BASE);
    s_req.data[7]  = (uint8_t)(size >> 24U);
    s_req.data[8]  = (uint8_t)(size >> 16U);
    s_req.data[9]  = (uint8_t)(size >> 8U);
    s_req.data[10] = (uint8_t)(size);
    s_req.length   = 11U;
    return uds_service_0x34_handler(&s_srv, &s_req, &s_resp);
}

/* Payload bytes per 0x36 block: maxNumberOfBlockLength - 2. */
#define PAYLOAD_LEN  (MOCK_BLOCK_LEN - 2U)

static uds_status_t send_0x36(uint8_t seq, uint32_t offset, uint32_t len)
{
    s_req.data[0] = 0x36U;
    s_req.data[1] = seq;
    (void)memcpy(&s_req.data[2], &s_image[offset], (size_t)len);
    s_req.length = (uint16_t)(2U + len);
    return uds_service_0x36_handler(&s_srv, &s_req, &s_resp);
}

static uds_status_t send_0x37(uint32_t crc)
{
    s_req.data[0] = 0x37U;
    s_req.data[1] = (uint8_t)(crc >> 24U);
    s_req.data[2] = (uint8_t)(crc >> 16U);
    s_req.data[3] = (uint8_t)(crc >> 8U);
    s_req.data[4] = (uint8_t)(crc);
    s_req.length  = 5U;
    return uds_service_0x37_handler(&s_srv, &s_req, &s_resp);
}

static uint32_t image_crc(uint32_t size)
{
    return uds_transfer_crc32_finalise(uds_transfer_crc32_update(0xFFFFFFFFUL, s_image, size));
}

/* Send every block of a size-byte image. Returns the first non-OK status
 * and the 1-based block number it came on (0 if all were accepted). */
static uds_status_t send_all_blocks(uint32_t size, uint32_t *failed_block)
{
    uint32_t     off = 0U;
    uint32_t     n   = 0U;
    uds_status_t st  = UDS_STATUS_OK;

    *failed_block = 0U;
    while ((off < size) && (st == UDS_STATUS_OK)) {
        uint32_t len = ((size - off) < PAYLOAD_LEN) ? (size - off) : PAYLOAD_LEN;
        n++;
        st = send_0x36((uint8_t)(n & 0xFFU), off, len);
        off += len;
    }
    if (st != UDS_STATUS_OK) {
        *failed_block = n;
    }
    return st;
}

void setUp(void)
{
    uint32_t i;

    memset(&s_sess, 0, sizeof(s_sess));
    memset(&s_sec,  0, sizeof(s_sec));
    memset(&s_srv,  0, sizeof(s_srv));
    memset(&s_req,  0, sizeof(s_req));
    memset(&s_resp, 0, sizeof(s_resp));
    memset(s_flash, 0xFF, sizeof(s_flash));
    for (i = 0U; i < MOCK_FLASH_SIZE; i++) {
        s_image[i] = (uint8_t)((i * 7U) ^ (i >> 8U));
    }

    s_corrupt_write_no  = NO_FAULT;
    s_step_read_fail    = false;
    s_step_skip_crc_out = false;
    s_write_calls       = 0U;
    s_step_calls        = 0U;
    s_step_bytes        = 0U;
    s_verify_calls      = 0U;

    (void)uds_safety_init();
    s_sess.initialized     = true;
    s_sess.active_session  = UDS_SESSION_PROGRAMMING;
    s_sec.initialized      = true;
    s_sec.active_level     = 1U;
    s_srv.cfg.session_ctx  = &s_sess;
    s_srv.cfg.security_ctx = &s_sec;

    uds_transfer_ctx_reset(uds_transfer_ctx_get());
    (void)uds_flash_ops_register(&k_ops_step);
}

void tearDown(void) {}

ZTEST_SUITE(svc_0x36_block_verify, NULL, NULL, NULL, NULL, NULL);

/* Image sizes: a whole number of 254-byte payloads is not a whole number of
 * 256-byte write chunks, so 4000 B leaves a partial chunk for 0x37 to flush. */
#define IMG_SIZE  (4000U)

/* TC-0x36-BV-001 */
ZTEST(svc_0x36_block_verify, test_clean_download_reads_back_per_block)
{
    uint32_t failed;

    zassert_equal(UDS_STATUS_OK, send_0x34(IMG_SIZE), "0x34");
    zassert_equal(UDS_STATUS_OK, send_all_blocks(IMG_SIZE, &failed), "block %u", failed);
    zassert_equal(UDS_STATUS_OK, send_0x37(image_crc(IMG_SIZE)), "0x37");
    zassert_equal(0x77U, s_resp.data[0], "");

    zassert_equal(s_write_calls, s_step_calls, "every write is read back");
    zassert_equal((uint32_t)IMG_SIZE, s_step_bytes, "the whole region is read back");
    zassert_equal(0U, s_verify_calls, "0x37 must not read the region back again");
    zassert_equal(0, memcmp(s_flash, s_image, IMG_SIZE), "");
}

/* TC-0x36-BV-002  Fail-first: before EDS#543 the corrupted block was answered
 * 0x76 and only the 0x37 readback caught it. */
ZTEST(svc_0x36_block_verify, test_corrupted_block_refused_at_its_0x36)
{
    uint32_t failed;

    s_corrupt_write_no = 5U;   /* 5th 256-byte write = bytes 1024..1279 */

    zassert_equal(UDS_STATUS_OK, send_0x34(IMG_SIZE), "0x34");
    zassert_equal(UDS_STATUS_ERR_PLATFORM, send_all_blocks(IMG_SIZE, &failed),
                  "a block that reads back wrong must be NRC 0x72");
    /* The 5th write completes during the 6th block (254-byte payloads). */
    zassert_equal(6U, failed, "refused at the 0x36 that wrote it, got block %u", failed);
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state, "transfer aborted");
    zassert_equal(UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE, send_0x37(image_crc(IMG_SIZE)),
                  "0x37 after the abort is NRC 0x24, never 0x77");
}

/* TC-0x36-BV-003  The last, partial chunk is flushed by 0x37 and read back there. */
ZTEST(svc_0x36_block_verify, test_corrupted_last_chunk_refused_at_0x37)
{
    uint32_t failed;

    s_corrupt_write_no = (IMG_SIZE / MOCK_BLOCK_LEN) + 1U;   /* the 0x37 flush */

    zassert_equal(UDS_STATUS_OK, send_0x34(IMG_SIZE), "0x34");
    zassert_equal(UDS_STATUS_OK, send_all_blocks(IMG_SIZE, &failed), "block %u", failed);
    zassert_equal(UDS_STATUS_ERR_PLATFORM, send_0x37(image_crc(IMG_SIZE)), "NRC 0x72");
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state, "");
}

/* TC-0x36-BV-004 */
ZTEST(svc_0x36_block_verify, test_readback_failure_refused)
{
    uint32_t failed;

    s_step_read_fail = true;
    zassert_equal(UDS_STATUS_OK, send_0x34(IMG_SIZE), "0x34");
    zassert_equal(UDS_STATUS_ERR_PLATFORM, send_all_blocks(IMG_SIZE, &failed), "");
    zassert_equal(2U, failed, "first write happens on block 2, got %u", failed);
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state, "");
}

/* TC-0x36-BV-005 */
ZTEST(svc_0x36_block_verify, test_step_without_crc_fails_closed)
{
    uint32_t failed;

    s_step_skip_crc_out = true;
    zassert_equal(UDS_STATUS_OK, send_0x34(IMG_SIZE), "0x34");
    zassert_equal(UDS_STATUS_ERR_PLATFORM, send_all_blocks(IMG_SIZE, &failed), "");
}

/* TC-0x36-BV-006  The region CRC-32 comparison at 0x37 is a real check: make
 * the read-back region's CRC differ from the received data's while both
 * running CRCs still agree with each other. */
ZTEST(svc_0x36_block_verify, test_region_crc_mismatch_refused_at_0x37)
{
    uint32_t            failed;
    uds_transfer_ctx_t *tctx = uds_transfer_ctx_get();

    zassert_equal(UDS_STATUS_OK, send_0x34(IMG_SIZE), "0x34");
    zassert_equal(UDS_STATUS_OK, send_all_blocks(IMG_SIZE, &failed), "block %u", failed);
    tctx->readback_crc_accumulator ^= 0x00010000UL;
    tctx->flushed_crc_accumulator  ^= 0x00010000UL;
    zassert_equal(UDS_STATUS_ERR_PLATFORM, send_0x37(image_crc(IMG_SIZE)), "NRC 0x72");
    zassert_equal(0U, s_verify_calls, "");
}

/* TC-0x36-BV-007 */
ZTEST(svc_0x36_block_verify, test_wrong_tester_crc_refused)
{
    uint32_t failed;

    zassert_equal(UDS_STATUS_OK, send_0x34(IMG_SIZE), "0x34");
    zassert_equal(UDS_STATUS_OK, send_all_blocks(IMG_SIZE, &failed), "block %u", failed);
    zassert_equal(UDS_STATUS_ERR_PLATFORM, send_0x37(image_crc(IMG_SIZE) ^ 1U), "NRC 0x72");
}

/* TC-0x36-BV-008  Flash ops without verify_step_cb keep the 0x37 readback. */
ZTEST(svc_0x36_block_verify, test_no_step_cb_uses_verify_cb)
{
    uint32_t failed;

    (void)uds_flash_ops_register(&k_ops_no_step);
    zassert_equal(UDS_STATUS_OK, send_0x34(IMG_SIZE), "0x34");
    zassert_equal(UDS_STATUS_OK, send_all_blocks(IMG_SIZE, &failed), "block %u", failed);
    zassert_equal(UDS_STATUS_OK, send_0x37(image_crc(IMG_SIZE)), "0x37");
    zassert_equal(1U, s_verify_calls, "");
    zassert_equal(0U, s_step_calls, "");

    /* And the corrupted block is still refused, at 0x37. */
    setUp();
    (void)uds_flash_ops_register(&k_ops_no_step);
    s_corrupt_write_no = 5U;
    zassert_equal(UDS_STATUS_OK, send_0x34(IMG_SIZE), "0x34");
    zassert_equal(UDS_STATUS_OK, send_all_blocks(IMG_SIZE, &failed), "block %u", failed);
    zassert_equal(UDS_STATUS_ERR_PLATFORM, send_0x37(image_crc(IMG_SIZE)), "NRC 0x72");
}

/* TC-0x36-BV-009  Region only partly read back -> whole-region verify_cb. */
ZTEST(svc_0x36_block_verify, test_ops_swapped_mid_transfer_falls_back)
{
    zassert_equal(UDS_STATUS_OK, send_0x34(IMG_SIZE), "0x34");
    zassert_equal(UDS_STATUS_OK, send_0x36(1U, 0U, PAYLOAD_LEN), "");
    zassert_equal(UDS_STATUS_OK, send_0x36(2U, PAYLOAD_LEN, PAYLOAD_LEN), "");  /* 1st write */
    (void)uds_flash_ops_register(&k_ops_no_step);
    {
        uint32_t off = 2U * PAYLOAD_LEN;
        uint8_t  seq = 3U;
        while (off < IMG_SIZE) {
            uint32_t len = ((IMG_SIZE - off) < PAYLOAD_LEN) ? (IMG_SIZE - off) : PAYLOAD_LEN;
            zassert_equal(UDS_STATUS_OK, send_0x36(seq, off, len), "");
            off += len;
            seq++;
        }
    }
    zassert_equal(UDS_STATUS_OK, send_0x37(image_crc(IMG_SIZE)), "0x37");
    zassert_equal(1U, s_step_calls, "");
    zassert_equal(1U, s_verify_calls, "partly read back: the whole region is checked");
}

/* TC-0x36-BV-010 */
ZTEST(svc_0x36_block_verify, test_repeated_block_not_read_back_twice)
{
    uint32_t steps;

    zassert_equal(UDS_STATUS_OK, send_0x34(IMG_SIZE), "0x34");
    zassert_equal(UDS_STATUS_OK, send_0x36(1U, 0U, PAYLOAD_LEN), "");
    zassert_equal(UDS_STATUS_OK, send_0x36(2U, PAYLOAD_LEN, PAYLOAD_LEN), "");
    steps = s_step_calls;
    zassert_equal(UDS_STATUS_OK, send_0x36(2U, PAYLOAD_LEN, PAYLOAD_LEN), "repeat");
    zassert_equal(steps, s_step_calls, "");
    zassert_equal(PAYLOAD_LEN * 2U - MOCK_BLOCK_LEN,
                  (uint32_t)uds_transfer_ctx_get()->write_buf_fill, "");
}

/* ==========================================================================
 * run_all_tests
 * ========================================================================== */

void run_all_tests(void)
{
    RUN_TEST(svc_0x36_block_verify__test_clean_download_reads_back_per_block);
    RUN_TEST(svc_0x36_block_verify__test_corrupted_block_refused_at_its_0x36);
    RUN_TEST(svc_0x36_block_verify__test_corrupted_last_chunk_refused_at_0x37);
    RUN_TEST(svc_0x36_block_verify__test_readback_failure_refused);
    RUN_TEST(svc_0x36_block_verify__test_step_without_crc_fails_closed);
    RUN_TEST(svc_0x36_block_verify__test_region_crc_mismatch_refused_at_0x37);
    RUN_TEST(svc_0x36_block_verify__test_wrong_tester_crc_refused);
    RUN_TEST(svc_0x36_block_verify__test_no_step_cb_uses_verify_cb);
    RUN_TEST(svc_0x36_block_verify__test_ops_swapped_mid_transfer_falls_back);
    RUN_TEST(svc_0x36_block_verify__test_repeated_block_not_read_back_twice);
}
