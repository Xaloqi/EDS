// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * FILE: platform/zephyr/zephyr_key_source.h
 *
 * PURPOSE: Zephyr binding of the SecurityAccess key source (EDS#56): picks the
 *          source from CONFIG_EDS_KEYS_PLACEHOLDER / _APP / _OTP and installs
 *          the level keys through platform/eds_key_source.h.
 */

#ifndef ZEPHYR_KEY_SOURCE_H
#define ZEPHYR_KEY_SOURCE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install the SecurityAccess keys from the configured source.
 *
 * Call once at start-up, before uds_generated_init().
 *
 * CONFIG_EDS_KEYS_PLACEHOLDER: changes nothing, logs the placeholder warning.
 * CONFIG_EDS_KEYS_APP        : keys from eds_app_get_level_key().
 * CONFIG_EDS_KEYS_OTP        : keys from otp_read() at the configured offsets
 *                              (needs Zephyr 4.4 or newer; the devicetree alias
 *                              eds-keys-otp names the OTP device).
 *
 * A key that cannot be read, or reads all 0x00 or all 0xFF, is refused and
 * logged; SecurityAccess then stays locked on every level.
 *
 * @return 0 if the keys of the configured source are in place (or the source is
 *         the placeholder), -1 if a key was refused and SecurityAccess is locked.
 */
int eds_zephyr_keys_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_KEY_SOURCE_H */
