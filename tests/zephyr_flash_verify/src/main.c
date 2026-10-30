// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * EDS#543: per-block readback during TransferData, on the real Zephyr flash
 * ops (platform/zephyr/zephyr_flash_ops.c) and native_sim's flash simulator.
 *
 * zephyr_flash_ops_init() registers the real table. Each test registers a
 * copy of it whose write_cb hands z_flash_write a copy of the data with one
 * bit flipped on the chosen call (fault injection: the driver stores the
 * wrong byte and reports success), and whose verify_cb counts calls before
 * calling the real one. Readback and erase are the real callbacks.
 * erase_step_cb is dropped so 0x34 erases in one call (no 0x78 to poll),
 * rounded up to the simulator's erase unit.
 */
#include <zephyr/ztest.h>
#include <string.h>

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_safety.h"
#include "uds_transfer_ctx.h"
#include "uds_flash_ops.h"
#include "zephyr_flash_ops.h"
#include "zephyr_wdt.h"

/* zephyr_flash_ops.c feeds the watchdog while it erases; none is set here. */
uds_status_t diag_wdt_feed(diag_wdt_t *wdt)
{
	ARG_UNUSED(wdt);
	return UDS_STATUS_OK;
}

/* Not under test, and the full server/safety modules are not linked here. */
void uds_safety_record_platform_violation(uds_status_t code)
{
	ARG_UNUSED(code);
}

uds_status_t uds_server_build_negative_response(uint8_t service_id, uds_nrc_t nrc,
						uds_msg_buf_t *resp)
{
	ARG_UNUSED(service_id);
	ARG_UNUSED(nrc);
	ARG_UNUSED(resp);
	return UDS_STATUS_OK;
}

#define IMG_SIZE     (64U * 1024U + 100U)   /* ends in a partial chunk for 0x37 */
#define PAYLOAD_LEN  (254U)                 /* max_block_length 256 - SID - counter */
#define NO_FAULT     (0xFFFFFFFFU)

static const uds_flash_ops_t *real;
static uds_flash_ops_t        ops;
static uint32_t               base;
static uint32_t               corrupt_write_no;
static uint32_t               write_calls;
static uint32_t               verify_calls;
static uint8_t                image[IMG_SIZE];
static uint8_t                wbuf[512];

static uds_session_ctx_t  sess;
static uds_security_ctx_t sec;
static uds_server_ctx_t   srv;
static uds_msg_buf_t      req;
static uds_msg_buf_t      resp;

static uds_status_t faulty_write(uint32_t address, const uint8_t *data, uint32_t length)
{
	write_calls++;
	if (write_calls == corrupt_write_no) {
		memcpy(wbuf, data, length);
		wbuf[length / 2U] ^= 0x10U;
		return real->write_cb(address, wbuf, length);
	}
	return real->write_cb(address, data, length);
}

/* The flash simulator refuses an erase that is not a whole number of its
 * 4 KiB erase units; the image deliberately is not. Erase is not under test. */
static uds_status_t aligned_erase(uint32_t address, uint32_t size)
{
	return real->erase_cb(address, ROUND_UP(size, 4096U));
}

static uds_status_t counting_verify(uint32_t address, uint32_t size, uint32_t crc)
{
	verify_calls++;
	return real->verify_cb(address, size, crc);
}

static uds_status_t send_0x34(uint32_t size)
{
	uint8_t r[] = {0x34, 0x00, 0x44,
		       (uint8_t)(base >> 24), (uint8_t)(base >> 16), (uint8_t)(base >> 8), (uint8_t)base,
		       (uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size};

	memcpy(req.data, r, sizeof(r));
	req.length = sizeof(r);
	return uds_service_0x34_handler(&srv, &req, &resp);
}

static uds_status_t send_0x37(uint32_t crc)
{
	uint8_t r[] = {0x37, (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8),
		       (uint8_t)crc};

	memcpy(req.data, r, sizeof(r));
	req.length = sizeof(r);
	return uds_service_0x37_handler(&srv, &req, &resp);
}

/* Sends every block; returns the first non-OK status, *failed = its block number. */
static uds_status_t send_blocks(uint32_t *failed)
{
	uint32_t off = 0U, n = 0U;
	uds_status_t st = UDS_STATUS_OK;

	*failed = 0U;
	while ((off < IMG_SIZE) && (st == UDS_STATUS_OK)) {
		uint32_t len = MIN(IMG_SIZE - off, PAYLOAD_LEN);

		n++;
		req.data[0] = 0x36;
		req.data[1] = (uint8_t)n;
		memcpy(&req.data[2], &image[off], len);
		req.length = (uint16_t)(2U + len);
		st = uds_service_0x36_handler(&srv, &req, &resp);
		off += len;
	}
	if (st != UDS_STATUS_OK) {
		*failed = n;
	}
	return st;
}

static uint32_t image_crc(void)
{
	return uds_transfer_crc32_finalise(uds_transfer_crc32_update(0xFFFFFFFFU, image, IMG_SIZE));
}

static void *suite_setup(void)
{
	zassert_equal(UDS_STATUS_OK, zephyr_flash_ops_init(), "zephyr_flash_ops_init");
	real = uds_flash_ops_get();
	zassert_not_null(real->verify_step_cb, "Zephyr flash ops must register verify_step_cb");
	base = real->memory_map[0].base_address;
	for (uint32_t i = 0U; i < IMG_SIZE; i++) {
		image[i] = (uint8_t)((i * 31U) ^ (i >> 9));
	}
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	ops = *real;
	ops.write_cb = faulty_write;
	ops.verify_cb = counting_verify;
	ops.erase_cb = aligned_erase;
	ops.erase_step_cb = NULL;
	zassert_equal(UDS_STATUS_OK, uds_flash_ops_register(&ops), "");

	corrupt_write_no = NO_FAULT;
	write_calls = 0U;
	verify_calls = 0U;
	memset(&sess, 0, sizeof(sess));
	memset(&sec, 0, sizeof(sec));
	memset(&srv, 0, sizeof(srv));
	sess.initialized = true;
	sess.active_session = UDS_SESSION_PROGRAMMING;
	sec.initialized = true;
	sec.active_level = 1U;
	srv.cfg.session_ctx = &sess;
	srv.cfg.security_ctx = &sec;
	uds_transfer_ctx_reset(uds_transfer_ctx_get());
}

ZTEST_SUITE(flash_verify, NULL, suite_setup, before, NULL, NULL);

ZTEST(flash_verify, test_clean_download_no_region_readback_at_exit)
{
	uint32_t failed = 0U;

	zassert_equal(UDS_STATUS_OK, send_0x34(IMG_SIZE), "0x34");
	zassert_equal(UDS_STATUS_OK, send_blocks(&failed), "block %u", failed);
	zassert_equal(UDS_STATUS_OK, send_0x37(image_crc()), "0x37");
	zassert_equal(0x77, resp.data[0], "");
	zassert_equal(0U, verify_calls, "0x37 must not read the whole region back");
	/* And the region really holds the image: the whole-region check agrees. */
	zassert_equal(UDS_STATUS_OK, real->verify_cb(base, IMG_SIZE, image_crc()), "");
}

ZTEST(flash_verify, test_corrupted_write_refused_at_its_0x36)
{
	uint32_t failed = 0U;

	corrupt_write_no = 40U;   /* bytes 9984..10239, completed by block 41 */
	zassert_equal(UDS_STATUS_OK, send_0x34(IMG_SIZE), "0x34");
	zassert_equal(UDS_STATUS_ERR_PLATFORM, send_blocks(&failed), "NRC 0x72");
	zassert_equal(41U, failed, "refused at the block that wrote it, got %u", failed);
	zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state, "aborted");
	zassert_not_equal(UDS_STATUS_OK, send_0x37(image_crc()), "never 0x77");
}

ZTEST(flash_verify, test_corrupted_last_chunk_refused_at_0x37)
{
	uint32_t failed = 0U;

	corrupt_write_no = (IMG_SIZE / 256U) + 1U;   /* the partial chunk 0x37 flushes */
	zassert_equal(UDS_STATUS_OK, send_0x34(IMG_SIZE), "0x34");
	zassert_equal(UDS_STATUS_OK, send_blocks(&failed), "block %u", failed);
	zassert_equal(UDS_STATUS_ERR_PLATFORM, send_0x37(image_crc()), "NRC 0x72");
}

ZTEST(flash_verify, test_without_step_cb_exit_still_refuses)
{
	uint32_t failed = 0U;

	ops.verify_step_cb = NULL;   /* the pre-EDS#543 path */
	corrupt_write_no = 40U;
	zassert_equal(UDS_STATUS_OK, send_0x34(IMG_SIZE), "0x34");
	zassert_equal(UDS_STATUS_OK, send_blocks(&failed), "block %u", failed);
	zassert_equal(UDS_STATUS_ERR_PLATFORM, send_0x37(image_crc()), "NRC 0x72");
	zassert_equal(1U, verify_calls, "");
}
