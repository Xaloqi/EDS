// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/eds_device_uid.h
 *
 * PURPOSE: The mapping behind eds_platform_get_device_uid() (EDS#57), kept
 *          free of any RTOS so a host test can run it with a mocked source.
 *          The Zephyr binding passes hwinfo_get_device_id(), the FreeRTOS
 *          board file its STM32 UID read.
 *
 *          A source returns the number of UID bytes it wrote (at most the
 *          len it was offered), or a negative errno: -ENOSYS means the
 *          hardware or driver has no UID, anything else is a fault.
 *
 * SAFETY  : Not safety-relevant: the UID identifies the silicon.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef EDS_DEVICE_UID_H
#define EDS_DEVICE_UID_H

#include "uds_types.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/** Longest UID any binding returns: 128 bits (MCX N94x). The STM32H7's is 96. */
#define EDS_DEVICE_UID_MAX_LEN 16U

typedef int32_t (*eds_device_uid_source_t)(uint8_t *buf, size_t len);

static inline uds_status_t eds_device_uid_read(eds_device_uid_source_t src, uint8_t *buf, size_t len)
{
    uint8_t tmp[EDS_DEVICE_UID_MAX_LEN] = {0};
    uds_status_t rc;

    if ((src == NULL) || (buf == NULL)) {
        rc = UDS_STATUS_ERR_NULL_PTR;
    } else if (len == 0U) {
        rc = UDS_STATUS_ERR_INVALID_PARAM;
    } else {
        const int32_t n = src(tmp, sizeof(tmp));
        if (n == -ENOSYS) {
            rc = UDS_STATUS_ERR_NOT_SUPPORTED;
        } else if ((n <= 0) || ((size_t)n > sizeof(tmp))) {
            rc = UDS_STATUS_ERR_PLATFORM;
        } else if ((size_t)n > len) {
            rc = UDS_STATUS_ERR_BUFFER_OVERFLOW;
        } else {
            size_t i;
            for (i = 0U; i < len; i++) {
                buf[i] = (i < (size_t)n) ? tmp[i] : 0U;
            }
            rc = UDS_STATUS_OK;
        }
    }
    return rc;
}

#endif /* EDS_DEVICE_UID_H */
