// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/zephyr_key_source.c
 *
 * PURPOSE: Zephyr binding of the SecurityAccess key source (EDS#56). The
 *          installation and its refusals are in platform/eds_key_source.h
 *          (host-tested); this file supplies the sources.
 *
 *          CONFIG_EDS_KEYS_OTP reads with otp_read() from the device the
 *          devicetree alias `eds-keys-otp` names, at CONFIG_EDS_KEYS_OTP_L1_OFFSET
 *          and CONFIG_EDS_KEYS_OTP_L2_OFFSET. The OTP API is Zephyr 4.4 or newer.
 *          Tested against Zephyr's OTP emulator (zephyr,otp-emul); not run
 *          against real fuses.
 *
 * SAFETY  : Security-relevant. Not safety-assessed.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "zephyr_key_source.h"
#include "eds_key_source.h"

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/version.h>

LOG_MODULE_REGISTER(zephyr_key_source, LOG_LEVEL_INF);

#if defined(CONFIG_EDS_KEYS_OTP)

#if ZEPHYR_VERSION_CODE < ZEPHYR_VERSION(4, 4, 0)
#pragma message("CONFIG_EDS_KEYS_OTP: this build is Zephyr " KERNEL_VERSION_STRING)
#error "CONFIG_EDS_KEYS_OTP needs Zephyr 4.4 or newer: it has the OTP API (<zephyr/drivers/otp.h>), and older Zephyr, including the v3.7 pin, does not. Use CONFIG_EDS_KEYS_APP, or build on Zephyr 4.5 (EDS#56)."
#elif !defined(CONFIG_OTP)
#error "CONFIG_EDS_KEYS_OTP needs CONFIG_OTP=y and an OTP driver for the board (EDS#56)."
#elif !DT_NODE_EXISTS(DT_ALIAS(eds_keys_otp))
#error "CONFIG_EDS_KEYS_OTP needs the devicetree alias eds-keys-otp pointing at the OTP node, e.g. aliases { eds-keys-otp = &otpc; }; (EDS#56)."
#elif (CONFIG_EDS_KEYS_OTP_L1_OFFSET < 0) || (CONFIG_EDS_KEYS_OTP_L2_OFFSET < 0)
#error "Set CONFIG_EDS_KEYS_OTP_L1_OFFSET and CONFIG_EDS_KEYS_OTP_L2_OFFSET to the byte offsets of the two AES-128 keys in your OTP fuse map: there is no default, an unset one would read arbitrary fuses as a key (EDS#56)."
#endif

#include <zephyr/drivers/otp.h>

static int otp_key_read(uint8_t level, uint8_t *key, void *ctx)
{
    const struct device *dev = (const struct device *)ctx;
    const off_t off = (level == 0x01U) ? (off_t)CONFIG_EDS_KEYS_OTP_L1_OFFSET
                                       : (off_t)CONFIG_EDS_KEYS_OTP_L2_OFFSET;

    return otp_read(dev, off, key, EDS_KEY_LEN);
}

#endif /* CONFIG_EDS_KEYS_OTP */

#if defined(CONFIG_EDS_KEYS_APP)
static int app_key_read(uint8_t level, uint8_t *key, void *ctx)
{
    (void)ctx;
    return eds_app_get_level_key(level, key);
}
#endif

#if defined(CONFIG_EDS_KEYS_APP) || defined(CONFIG_EDS_KEYS_OTP)
static int install(eds_key_read_fn_t read, void *ctx, const char *source)
{
    static const char *const why[] = {"installed", "read failed", "all 0x00 (unprogrammed)",
                                      "all 0xFF (erased)"};
    eds_key_scratch_t scratch;
    eds_key_state_t st[EDS_KEY_LEVELS];
    size_t i;
    const bool ok = eds_keys_install(read, ctx, &scratch, st);

    for (i = 0U; i < EDS_KEY_LEVELS; i++) {
        if (st[i] == EDS_KEY_INSTALLED) {
            LOG_INF("[P1-SEC] level 0x%02X key from %s: installed", EDS_KEY_LEVEL_IDS[i], source);
        } else {
            LOG_ERR("[P1-SEC] level 0x%02X key from %s: REFUSED, %s", EDS_KEY_LEVEL_IDS[i], source,
                    why[st[i]]);
        }
    }
    if (!ok) {
        LOG_ERR("[P1-SEC] SecurityAccess stays LOCKED on every level.");
        return -1;
    }
    return 0;
}
#endif

int eds_zephyr_keys_init(void)
{
#if defined(CONFIG_EDS_KEYS_OTP)
    const struct device *dev = DEVICE_DT_GET(DT_ALIAS(eds_keys_otp));

    if (!device_is_ready(dev)) {
        LOG_ERR("[P1-SEC] OTP device not ready: SecurityAccess stays LOCKED on every level.");
        uds_security_algo_set_derive_cb(eds_keys_deny_all_derive);
        return -1;
    }
    return install(otp_key_read, (void *)dev, "OTP");
#elif defined(CONFIG_EDS_KEYS_APP)
    return install(app_key_read, NULL, "the application");
#else
    LOG_WRN("[P1-SEC] Using compile-time placeholder AES keys. "
            "Select CONFIG_EDS_KEYS_APP or CONFIG_EDS_KEYS_OTP before production.");
    return 0;
#endif
}
