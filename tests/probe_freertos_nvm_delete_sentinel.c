// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: tests/probe_freertos_nvm_delete_sentinel.c
 *
 * PURPOSE: Standalone behavioural probe for platform/freertos/freertos_nvm.c
 *          — the one nvm_store_* backend that build_tests.sh's shared
 *          DIAG_STACK_SRCS never links (every Unity test module in
 *          tests/unit_runnable/ links platform/zephyr/nvm_store_mock.c
 *          instead, via one shared stack object library — adding this file
 *          there too would collide on duplicate nvm_store_* symbol
 *          definitions). Compiled and RUN directly by build_tests.sh,
 *          exactly like tests/probe_eds_build_is_production.c is compiled
 *          (but not run — it probes a macro via #error) by
 *          scripts/verify_build_mode_macro.sh; this is that same
 *          "standalone driver outside the shared stack" pattern, applied
 *          to a runtime behavioural check instead of a compile-time one.
 *
 * PROVES:
 *   1. [#285] nvm_store_delete() followed by nvm_store_read() on this
 *      backend reports UDS_STATUS_ERR_DID_NOT_FOUND, not
 *      UDS_STATUS_OK-with-the-raw-sentinel-byte. This backend has no
 *      native delete primitive and implements delete as a sentinel write
 *      (NVM_STORE_DELETE_SENTINEL_LEN/_BYTE, platform/nvm_store.h);
 *      nvm_store_read() must translate that back to DID_NOT_FOUND itself
 *      so this backend's read/delete pair honours the same contract every
 *      other backend gets for free from a true delete.
 *   2. [#280] nvm_store_erase_all() preserves NVM_KEY_SEC_STATE intact
 *      while genuinely clobbering an ordinary key, on this backend
 *      specifically (previously unverified anywhere but CI compile — the
 *      shared Unity suite only proves this against
 *      platform/zephyr/nvm_store_mock.c). Does NOT assert the ordinary
 *      key reads back as DID_NOT_FOUND — this backend's erase_all()
 *      wipes it via the same 1-byte sentinel nvm_store_delete() uses,
 *      and the #285 fix is deliberately scoped to NVM_KEY_SEC_STATE only
 *      (see nvm_store_read()'s doc comment), so an ordinary key correctly
 *      reads back as UDS_STATUS_OK with the sentinel's 1 stale byte, not
 *      DID_NOT_FOUND — a pre-existing characteristic, not something
 *      #280 or #285 changed or need to change.
 *
 * A minimal RAM-backed eds_nvm_ops_t stands in for a customer's flash
 * driver — enough to prove freertos_nvm.c's own logic, not a flash
 * driver's correctness (irrelevant here; freertos_nvm.c never touches
 * flash directly).
 *
 * Exit code 0 = every check passed. Any assertion failure aborts with a
 * message naming the specific check via stderr/exit status, exactly like
 * every other gate in build_tests.sh.
 * =============================================================================
 */

#include "nvm_store.h"
#include "platform_api.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* freertos_nvm_register_ops() is internal (declared in
 * platform/freertos/freertos_platform_api.c, not in the public
 * platform_api.h) — forward-declared here for the same reason
 * eds_platform_init() reaches it in production: this probe plays the role
 * of the platform-init glue code, not of application/UDS-layer code. */
void freertos_nvm_register_ops(const eds_nvm_ops_t *ops);

#define PROBE_SLOTS (8U)

typedef struct {
    uint16_t key;
    uint8_t  data[64];
    size_t   len;
    int      used;
} probe_slot_t;

static probe_slot_t s_slots[PROBE_SLOTS];

static uds_status_t probe_read(uint16_t key, uint8_t *buf, size_t len, size_t *out_len)
{
    for (unsigned i = 0U; i < PROBE_SLOTS; i++) {
        if (s_slots[i].used && (s_slots[i].key == key)) {
            size_t n = (s_slots[i].len < len) ? s_slots[i].len : len;
            (void)memcpy(buf, s_slots[i].data, n);
            if (out_len != NULL) {
                *out_len = n;
            }
            return UDS_STATUS_OK;
        }
    }
    return UDS_STATUS_ERR_DID_NOT_FOUND;
}

static uds_status_t probe_write(uint16_t key, const uint8_t *buf, size_t len)
{
    for (unsigned i = 0U; i < PROBE_SLOTS; i++) {
        if (s_slots[i].used && (s_slots[i].key == key)) {
            (void)memcpy(s_slots[i].data, buf, len);
            s_slots[i].len = len;
            return UDS_STATUS_OK;
        }
    }
    for (unsigned i = 0U; i < PROBE_SLOTS; i++) {
        if (!s_slots[i].used) {
            s_slots[i].used = 1;
            s_slots[i].key  = key;
            (void)memcpy(s_slots[i].data, buf, len);
            s_slots[i].len = len;
            return UDS_STATUS_OK;
        }
    }
    return UDS_STATUS_ERR_PLATFORM;
}

static bool probe_is_ready(void)
{
    return true;
}

int main(void)
{
    eds_nvm_ops_t ops = { .read = probe_read, .write = probe_write, .is_ready = probe_is_ready };
    uint8_t       rec[12];
    uint8_t       readback[12];
    uint8_t       after_delete[12];
    uint8_t       other[4] = { 1U, 2U, 3U, 4U };
    size_t        out_len  = 0U;
    uds_status_t  rc;

    freertos_nvm_register_ops(&ops);

    rc = nvm_store_init(NULL);
    assert(rc == UDS_STATUS_OK);

    /* Round-trip a real, fixed-size record (stand-in for NVM_KEY_SEC_STATE's
     * actual 12-byte UDS_SECURITY_NVM_RECORD_BYTES shape — this probe does
     * not depend on core/uds_security_nvm.h to stay a pure platform/-layer
     * check). */
    (void)memset(rec, 0xABU, sizeof(rec));
    rc = nvm_store_write(NVM_KEY_SEC_STATE, rec, sizeof(rec));
    assert(rc == UDS_STATUS_OK);

    (void)memset(readback, 0U, sizeof(readback));
    rc = nvm_store_read(NVM_KEY_SEC_STATE, readback, sizeof(readback), &out_len);
    assert(rc == UDS_STATUS_OK);
    assert(out_len == sizeof(rec));
    assert(memcmp(rec, readback, sizeof(rec)) == 0);

    /* [#285] THE check this probe exists for: delete() then read() must
     * report DID_NOT_FOUND, not OK-with-the-1-byte-sentinel. */
    rc = nvm_store_delete(NVM_KEY_SEC_STATE);
    assert(rc == UDS_STATUS_OK);

    (void)memset(after_delete, 0xFFU, sizeof(after_delete));
    out_len = 999U;
    rc = nvm_store_read(NVM_KEY_SEC_STATE, after_delete, sizeof(after_delete), &out_len);
    if (rc != UDS_STATUS_ERR_DID_NOT_FOUND) {
        (void)fprintf(stderr,
            "[#285 PROBE FAIL] nvm_store_read() after nvm_store_delete() "
            "returned rc=%d (expected UDS_STATUS_ERR_DID_NOT_FOUND=%d)\n",
            (int)rc, (int)UDS_STATUS_ERR_DID_NOT_FOUND);
        return 1;
    }

    /* [#280] erase_all() preserves NVM_KEY_SEC_STATE, wipes an ordinary key,
     * on THIS backend specifically. */
    rc = nvm_store_write(NVM_KEY_SEC_STATE, rec, sizeof(rec));
    assert(rc == UDS_STATUS_OK);
    rc = nvm_store_write(NVM_KEY_LIFECYCLE_CNT, other, sizeof(other));
    assert(rc == UDS_STATUS_OK);

    rc = nvm_store_erase_all();
    assert(rc == UDS_STATUS_OK);

    (void)memset(readback, 0U, sizeof(readback));
    rc = nvm_store_read(NVM_KEY_SEC_STATE, readback, sizeof(readback), &out_len);
    if ((rc != UDS_STATUS_OK) || (memcmp(rec, readback, sizeof(rec)) != 0)) {
        (void)fprintf(stderr,
            "[#280 PROBE FAIL] NVM_KEY_SEC_STATE did not survive "
            "nvm_store_erase_all() on the FreeRTOS backend (rc=%d)\n",
            (int)rc);
        return 1;
    }

    /* NOTE: this backend's erase_all() wipes an ordinary key by
     * overwriting it with the same 1-byte sentinel nvm_store_delete()
     * uses — pre-existing behaviour, unrelated to #280/#285, and NOT
     * translated back to DID_NOT_FOUND for this key (the #285
     * translation in nvm_store_read() is deliberately scoped to
     * NVM_KEY_SEC_STATE only — see that function's doc comment). The
     * guarantee this backend actually provides for an ordinary key is
     * "the original value no longer reads back intact", which is what
     * #280 needs: proof that SEC_STATE is excluded from the SAME wipe
     * that genuinely does clobber everything else. */
    (void)memset(other, 0xEEU, sizeof(other));
    rc = nvm_store_read(NVM_KEY_LIFECYCLE_CNT, other, sizeof(other), &out_len);
    if ((rc == UDS_STATUS_OK) && (out_len == 4U) &&
        (other[0] == 1U) && (other[1] == 2U) && (other[2] == 3U) && (other[3] == 4U)) {
        (void)fprintf(stderr,
            "[#280 PROBE FAIL] NVM_KEY_LIFECYCLE_CNT's original value "
            "survived nvm_store_erase_all() intact on the FreeRTOS "
            "backend (rc=%d) -- erase_all() did not touch it at all\n",
            (int)rc);
        return 1;
    }

    (void)printf("freertos_nvm probe: all checks passed (#280 erase_all "
                 "exclusion, #285 delete-sentinel translation)\n");
    return 0;
}
