// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_device_uid.c
 *
 * MODULE UNDER TEST: platform/eds_device_uid.h — eds_device_uid_read(), the
 *                    mapping behind eds_platform_get_device_uid() (EDS#57).
 *
 * PURPOSE:
 *   The Zephyr binding hands it hwinfo_get_device_id(), the FreeRTOS board
 *   file its STM32 UID read. Here a mocked source stands in for both: the
 *   test checks what the caller gets for a short UID, an exact fit, a buffer
 *   that is too small, a driver without the feature (-ENOSYS) and an I/O
 *   error. A UID is never truncated: a short buffer is an error, not a
 *   shorter identifier that two devices could share.
 *
 * FRAMEWORK: Zephyr Ztest (shim used for host builds — see tests/runner/)
 * BUILT BY : build_tests.sh, tests/CMakeLists.txt
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "platform_api.h"
#include "eds_device_uid.h"

static int32_t g_src_rc;
static uint8_t g_src_bytes[EDS_DEVICE_UID_MAX_LEN];
static size_t  g_src_calls;
static size_t  g_src_len_offered;

/* Mocked hwinfo_get_device_id(): copies up to `len` bytes of g_src_bytes the
 * way the driver does and returns the count, or g_src_rc when it is negative. */
static int32_t mock_hwinfo(uint8_t *buf, size_t len)
{
    g_src_calls++;
    g_src_len_offered = len;
    if (g_src_rc < 0) {
        return g_src_rc;
    }
    size_t n = ((size_t)g_src_rc < len) ? (size_t)g_src_rc : len;
    memcpy(buf, g_src_bytes, n);
    return (int32_t)n;
}

static void arm(int32_t rc)
{
    g_src_rc = rc;
    g_src_calls = 0U;
    g_src_len_offered = 0U;
    for (size_t i = 0U; i < sizeof(g_src_bytes); i++) {
        g_src_bytes[i] = (uint8_t)(0xA0U + i);
    }
}

/* The API exists in platform_api.h with the shape #57 asks for. */
ZTEST(device_uid_suite, test_api_is_declared)
{
    uds_status_t (*fn)(uint8_t *, size_t) = eds_platform_get_device_uid;
    TEST_ASSERT_NOT_NULL(fn);
}

/* STM32H7: 12 bytes into a 16-byte buffer: the 12 bytes, the rest zeroed. */
ZTEST(device_uid_suite, test_short_uid_is_zero_padded)
{
    uint8_t out[16];
    memset(out, 0x55, sizeof(out));
    arm(12);
    TEST_ASSERT_EQUAL_INT(UDS_STATUS_OK, eds_device_uid_read(mock_hwinfo, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(0, memcmp(g_src_bytes, out, 12));
    for (size_t i = 12U; i < sizeof(out); i++) {
        TEST_ASSERT_EQUAL_UINT8(0U, out[i]);
    }
}

/* MCX N94x: 16 bytes into a 16-byte buffer. */
ZTEST(device_uid_suite, test_exact_fit)
{
    uint8_t out[16];
    arm(16);
    TEST_ASSERT_EQUAL_INT(UDS_STATUS_OK, eds_device_uid_read(mock_hwinfo, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(0, memcmp(g_src_bytes, out, 16));
}

/* A 12-byte buffer cannot hold a 16-byte UID: refused, buffer untouched. */
ZTEST(device_uid_suite, test_buffer_too_small_is_refused_not_truncated)
{
    uint8_t out[12];
    memset(out, 0x55, sizeof(out));
    arm(16);
    TEST_ASSERT_EQUAL_INT(UDS_STATUS_ERR_BUFFER_OVERFLOW, eds_device_uid_read(mock_hwinfo, out, sizeof(out)));
    for (size_t i = 0U; i < sizeof(out); i++) {
        TEST_ASSERT_EQUAL_UINT8(0x55U, out[i]);
    }
}

/* The source is always offered the full EDS_DEVICE_UID_MAX_LEN, whatever the caller's len. */
ZTEST(device_uid_suite, test_source_is_offered_the_maximum)
{
    uint8_t out[32];
    arm(12);
    (void)eds_device_uid_read(mock_hwinfo, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(EDS_DEVICE_UID_MAX_LEN, (uint32_t)g_src_len_offered);
}

/* hwinfo without the feature (-ENOSYS) is NOT_SUPPORTED, not a fault. */
ZTEST(device_uid_suite, test_no_driver_is_not_supported)
{
    uint8_t out[16];
    arm(-ENOSYS);
    TEST_ASSERT_EQUAL_INT(UDS_STATUS_ERR_NOT_SUPPORTED, eds_device_uid_read(mock_hwinfo, out, sizeof(out)));
}

/* Any other driver error is a platform fault. */
ZTEST(device_uid_suite, test_io_error_is_platform_error)
{
    uint8_t out[16];
    arm(-EIO);
    TEST_ASSERT_EQUAL_INT(UDS_STATUS_ERR_PLATFORM, eds_device_uid_read(mock_hwinfo, out, sizeof(out)));
}

/* A driver that returns 0 bytes has not produced an identifier. */
ZTEST(device_uid_suite, test_empty_uid_is_platform_error)
{
    uint8_t out[16];
    arm(0);
    TEST_ASSERT_EQUAL_INT(UDS_STATUS_ERR_PLATFORM, eds_device_uid_read(mock_hwinfo, out, sizeof(out)));
}

ZTEST(device_uid_suite, test_bad_arguments)
{
    uint8_t out[16];
    arm(12);
    TEST_ASSERT_EQUAL_INT(UDS_STATUS_ERR_NULL_PTR, eds_device_uid_read(mock_hwinfo, NULL, 16U));
    TEST_ASSERT_EQUAL_INT(UDS_STATUS_ERR_NULL_PTR, eds_device_uid_read(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(UDS_STATUS_ERR_INVALID_PARAM, eds_device_uid_read(mock_hwinfo, out, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)g_src_calls);
}

/* ============================================================================
 * Unity runner infrastructure
 * ========================================================================= */

void setUp(void)    { }
void tearDown(void) { }

void run_all_tests(void)
{
    RUN_TEST(device_uid_suite__test_api_is_declared);
    RUN_TEST(device_uid_suite__test_short_uid_is_zero_padded);
    RUN_TEST(device_uid_suite__test_exact_fit);
    RUN_TEST(device_uid_suite__test_buffer_too_small_is_refused_not_truncated);
    RUN_TEST(device_uid_suite__test_source_is_offered_the_maximum);
    RUN_TEST(device_uid_suite__test_no_driver_is_not_supported);
    RUN_TEST(device_uid_suite__test_io_error_is_platform_error);
    RUN_TEST(device_uid_suite__test_empty_uid_is_platform_error);
    RUN_TEST(device_uid_suite__test_bad_arguments);
}
