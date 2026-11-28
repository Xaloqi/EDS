// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * EDS#56: eds_zephyr_keys_init() with CONFIG_EDS_KEYS_OTP against Zephyr's OTP
 * emulator. The emulator is RAM, zero at power-up: the keys are "programmed"
 * with otp_program() the way a provisioning step would write them.
 *
 * What it shows: keys read from the OTP node are the keys SecurityAccess uses
 * (not the placeholder keys); an unprogrammed (all-zero) or erased (all-0xFF)
 * key is refused, and a refusal locks every level.
 */

#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/otp.h>
#include <string.h>

#include "eds_key_source.h"
#include "uds_aes_cmac.h"
#include "uds_security_algo.h"
#include "zephyr_key_source.h"

#define OTP_DEV DEVICE_DT_GET(DT_ALIAS(eds_keys_otp))

/* Under CONFIG_EDS_KEYS_PLACEHOLDER (how safeboot_ecu behaved before #56) the
 * offsets do not exist; the test then fails, which is the fail-first record. */
#ifndef CONFIG_EDS_KEYS_OTP_L1_OFFSET
#define CONFIG_EDS_KEYS_OTP_L1_OFFSET 0
#define CONFIG_EDS_KEYS_OTP_L2_OFFSET 16
#endif

static const uint8_t SEED[UDS_ALGO_SEED_LEN] = {1, 2, 3, 4, 5, 6, 7, 8};

static void program(off_t off, uint8_t first, uint8_t step)
{
	uint8_t k[EDS_KEY_LEN];

	for (size_t i = 0; i < sizeof(k); i++) {
		k[i] = (uint8_t)(first + step * i);
	}
	zassert_equal(0, otp_program(OTP_DEV, off, k, sizeof(k)));
}

static void fill(off_t off, uint8_t v)
{
	uint8_t k[EDS_KEY_LEN];

	memset(k, v, sizeof(k));
	zassert_equal(0, otp_program(OTP_DEV, off, k, sizeof(k)));
}

/* AES-CMAC of SEED under the key at `off` in the OTP, truncated to the key response length. */
static void want_from_otp(off_t off, uint8_t out[UDS_ALGO_KEY_LEN])
{
	uint8_t key[EDS_KEY_LEN], mac[UDS_CMAC_TAG_LEN];

	zassert_equal(0, otp_read(OTP_DEV, off, key, sizeof(key)));
	zassert_equal(0, uds_aes_cmac(key, SEED, UDS_ALGO_SEED_LEN, mac));
	memcpy(out, mac, UDS_ALGO_KEY_LEN);
}

static void before(void *f)
{
	ARG_UNUSED(f);
	uds_security_algo_reset();
	zassert_true(device_is_ready(OTP_DEV));
}

ZTEST_SUITE(key_source_otp, NULL, NULL, before, NULL, NULL);

/* Power-up state of the emulator: all zero. Both keys refused, every level locked. */
ZTEST(key_source_otp, test_01_unprogrammed_otp_is_refused)
{
	uint8_t got[UDS_ALGO_KEY_LEN];

	zassert_equal(-1, eds_zephyr_keys_init());
	zassert_not_equal(UDS_STATUS_OK, uds_security_algo_derive_key(0x01U, SEED, got));
	zassert_not_equal(UDS_STATUS_OK, uds_security_algo_derive_key(0x03U, SEED, got));
	zassert_true(uds_security_algo_keys_are_placeholder());
}

/* Keys in the emulator are the keys SecurityAccess answers with. */
ZTEST(key_source_otp, test_02_keys_from_the_emulator_are_used)
{
	uint8_t got[UDS_ALGO_KEY_LEN], want[UDS_ALGO_KEY_LEN];

	program(CONFIG_EDS_KEYS_OTP_L1_OFFSET, 0xC0, 1);
	program(CONFIG_EDS_KEYS_OTP_L2_OFFSET, 0x30, 3);

	zassert_equal(0, eds_zephyr_keys_init());
	zassert_false(uds_security_algo_keys_are_placeholder());

	zassert_equal(UDS_STATUS_OK, uds_security_algo_derive_key(0x01U, SEED, got));
	want_from_otp(CONFIG_EDS_KEYS_OTP_L1_OFFSET, want);
	zassert_mem_equal(got, want, sizeof(got));

	zassert_equal(UDS_STATUS_OK, uds_security_algo_derive_key(0x03U, SEED, got));
	want_from_otp(CONFIG_EDS_KEYS_OTP_L2_OFFSET, want);
	zassert_mem_equal(got, want, sizeof(got));
}

/* One key all-zero: refused, and the level whose key is fine is locked too. */
ZTEST(key_source_otp, test_03_all_zero_key_is_refused)
{
	uint8_t got[UDS_ALGO_KEY_LEN];

	program(CONFIG_EDS_KEYS_OTP_L1_OFFSET, 0xC0, 1);
	fill(CONFIG_EDS_KEYS_OTP_L2_OFFSET, 0x00);

	zassert_equal(-1, eds_zephyr_keys_init());
	zassert_not_equal(UDS_STATUS_OK, uds_security_algo_derive_key(0x01U, SEED, got));
	zassert_not_equal(UDS_STATUS_OK, uds_security_algo_derive_key(0x03U, SEED, got));
}

/* An erased (all-0xFF) key is refused the same way. */
ZTEST(key_source_otp, test_04_all_ff_key_is_refused)
{
	uint8_t got[UDS_ALGO_KEY_LEN];

	fill(CONFIG_EDS_KEYS_OTP_L1_OFFSET, 0xFF);
	program(CONFIG_EDS_KEYS_OTP_L2_OFFSET, 0x30, 3);

	zassert_equal(-1, eds_zephyr_keys_init());
	zassert_not_equal(UDS_STATUS_OK, uds_security_algo_derive_key(0x03U, SEED, got));
}
