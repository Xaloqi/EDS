// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_nvm_store_zephyr_ops.c
 *
 * MODULE UNDER TEST: platform/zephyr/nvm_store.c — eds_nvm_ops_t support
 * (issue #304, Stage 1).
 *
 * WHY THIS EXISTS:
 *   platform/platform_api.h's eds_platform_cfg_t.nvm field is documented
 *   "Ignored on Zephyr" — platform/freertos/freertos_nvm.c already routes
 *   every nvm_store_* call through customer-supplied eds_nvm_ops_t callbacks
 *   when registered (falling back to a RAM stub otherwise), but
 *   platform/zephyr/nvm_store.c has no equivalent: it hardwires Zephyr NVS
 *   unconditionally. That asymmetry means a Zephyr integrator whose flash
 *   needs different treatment than built-in NVS (e.g. STM32H7's 128 KB /
 *   up-to-4000ms-erase sectors, #304's original defect) has no supported way
 *   to plug in their own driver short of writing an entire nvm_store.c
 *   replacement and re-wiring the example's CMakeLists.txt by hand
 *   (platform/zephyr/nvm_store_append.c is exactly that — a whole second
 *   compilation unit, selected at build time).
 *
 *   This test proves zephyr_nvm_store_register_ops() + the ops-dispatch
 *   path in nvm_store_init/write/read/delete/erase_all/is_ready, mirroring
 *   freertos_nvm.c's existing contract exactly (including the
 *   NVM_KEY_SEC_STATE delete-sentinel translation, and the explicit-
 *   allowlist erase_all() that never clears NVM_KEY_SEC_STATE, per #280).
 *
 *   BEFORE this fix: platform/zephyr/nvm_store.c's entire body (everything
 *   from zephyr_nvm_store_register_ops() to nvm_store_is_ready()) was
 *   wrapped in a single `#ifndef NVM_STORE_HOST_MOCK` guard with nothing
 *   else defined for the host-mock build — none of these symbols exist at
 *   all under NVM_STORE_HOST_MOCK=1. This test, linked directly against
 *   platform/zephyr/nvm_store.c (see tests/CMakeLists.txt /
 *   build_tests.sh's direct-link special case, the same pattern
 *   test_nvm_store_append.c uses), fails to LINK against the pre-fix file —
 *   "undefined reference to zephyr_nvm_store_register_ops" / "...to
 *   nvm_store_init" etc. — which names the real defect directly: the
 *   registration entry point this Stage requires simply did not exist.
 *   Confirmed by building this test against the unfixed tree before writing
 *   the fix (this repo's standing regression-test discipline).
 *
 * "NEVER CALLED" GUARDS: EDS's zassert_* macros (ztest_shim.h) do not abort
 * the current test on failure the way a direct Unity TEST_ASSERT at the top
 * level does when the assertion fails from a NESTED call (the fake ops
 * callback below is called via a function pointer from inside the module
 * under test, not directly from the test body) — so "was this callback
 * actually invoked" is proven by an incremented counter checked from the
 * test body afterwards, never by asserting inside the callback itself.
 *
 * Coverage:
 *   TC-001  No ops registered -> nvm_store_init() reports NOT_INITIALIZED
 *           on this host build (there is no real NVS to fall back to here;
 *           proves the ops path is not silently assumed)
 *   TC-002  Ops registered -> nvm_store_init() succeeds without needing a
 *           real flash_dev/cfg at all (cfg is accepted but unused on this
 *           path, matching platform/freertos/freertos_nvm.c)
 *   TC-003  nvm_store_write() actually invokes the registered ops.write —
 *           the callback-counter "never called" guard described above
 *   TC-004  nvm_store_read() actually invokes the registered ops.read and
 *           returns its data
 *   TC-005  nvm_store_delete() prefers ops.remove when present (native
 *           delete, [#287])
 *   TC-006  nvm_store_delete() falls back to the documented sentinel write
 *           via ops.write when ops.remove is NULL, and nvm_store_read()
 *           translates that sentinel back to DID_NOT_FOUND for
 *           NVM_KEY_SEC_STATE — the exact contract
 *           platform/freertos/freertos_nvm.c already honours ([#285])
 *   TC-007  nvm_store_is_ready() delegates to ops.is_ready()
 *   TC-008  nvm_store_erase_all() wipes every registered key except
 *           NVM_KEY_SEC_STATE (explicit allowlist, [#280] — matches
 *           platform/freertos/freertos_nvm.c's erase_all(), not
 *           platform/zephyr/nvm_store.c's own NVS nvs_clear()-and-restore
 *           approach, since an arbitrary customer ops table has no
 *           equivalent to nvs_clear())
 *   TC-009  Re-registering ops with NULL clears them back to "not
 *           registered" (zephyr_nvm_store_register_ops(NULL) — the
 *           counterpart to freertos_nvm_register_ops(NULL))
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h for host compilation)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "nvm_store.h"
#include "platform_api.h"

/* Forward declaration — implemented in platform/zephyr/nvm_store.c. Not
 * part of the public nvm_store_* API (nvm_store.h); called only from
 * platform/zephyr/zephyr_platform_api.c's eds_platform_init() in
 * production, and directly by this test on host. Mirrors
 * platform/freertos/freertos_platform_api.c's identical forward
 * declaration of freertos_nvm_register_ops(). */
void zephyr_nvm_store_register_ops(const eds_nvm_ops_t *ops);

/* ==========================================================================
 * Fake ops — a tiny in-RAM key/value table standing in for a customer's
 * real flash driver. Deliberately NOT the same shape as nvm_store_mock.c's
 * RAM stub: this one counts calls, so tests can prove dispatch happened
 * without asserting inside a callback (see file header).
 * ========================================================================== */

#define FAKE_MAX_RECORDS (8U)

typedef struct {
    uint16_t key;
    uint8_t  data[NVM_MAX_RECORD_BYTES];
    size_t   len;
    bool     in_use;
} fake_record_t;

static fake_record_t s_fake_store[FAKE_MAX_RECORDS];
static uint32_t      s_fake_write_calls;
static uint32_t      s_fake_read_calls;
static uint32_t      s_fake_is_ready_calls;
static uint32_t      s_fake_remove_calls;
static bool           s_fake_remove_enabled;

static fake_record_t *fake_find(uint16_t key)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)FAKE_MAX_RECORDS; i++) {
        if (s_fake_store[i].in_use && (s_fake_store[i].key == key)) {
            return &s_fake_store[i];
        }
    }
    return NULL;
}

static uds_status_t fake_write(uint16_t key, const uint8_t *buf, size_t len)
{
    fake_record_t *rec = fake_find(key);
    uint32_t i;

    s_fake_write_calls++;

    if (rec == NULL) {
        for (i = 0U; i < (uint32_t)FAKE_MAX_RECORDS; i++) {
            if (!s_fake_store[i].in_use) {
                rec = &s_fake_store[i];
                rec->in_use = true;
                rec->key    = key;
                break;
            }
        }
    }
    if (rec == NULL) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    (void)memcpy(rec->data, buf, len);
    rec->len = len;
    return UDS_STATUS_OK;
}

static uds_status_t fake_read(uint16_t key, uint8_t *buf, size_t len, size_t *out_len)
{
    fake_record_t *rec = fake_find(key);

    s_fake_read_calls++;

    if (rec == NULL) {
        return UDS_STATUS_ERR_DID_NOT_FOUND;
    }

    {
        size_t copy_len = (rec->len < len) ? rec->len : len;

        (void)memcpy(buf, rec->data, copy_len);
        if (out_len != NULL) {
            *out_len = rec->len;
        }
    }
    return UDS_STATUS_OK;
}

static bool fake_is_ready(void)
{
    s_fake_is_ready_calls++;
    return true;
}

static uds_status_t fake_remove(uint16_t key)
{
    fake_record_t *rec = fake_find(key);

    s_fake_remove_calls++;

    if (rec != NULL) {
        rec->in_use = false;
    }
    return UDS_STATUS_OK;
}

static void fake_store_reset(void)
{
    (void)memset(s_fake_store, 0, sizeof(s_fake_store));
    s_fake_write_calls    = 0U;
    s_fake_read_calls     = 0U;
    s_fake_is_ready_calls = 0U;
    s_fake_remove_calls   = 0U;
    s_fake_remove_enabled = false;
}

static uds_status_t fake_remove_dispatch(uint16_t key)
{
    return fake_remove(key);
}

static void fake_ops_register(bool with_remove)
{
    eds_nvm_ops_t ops = {
        .read     = fake_read,
        .write    = fake_write,
        .is_ready = fake_is_ready,
        .remove   = with_remove ? fake_remove_dispatch : NULL,
    };

    s_fake_remove_enabled = with_remove;
    zephyr_nvm_store_register_ops(&ops);
}

/* ==========================================================================
 * setUp / tearDown
 * ========================================================================== */

void setUp(void)
{
    fake_store_reset();
    nvm_mock_reset();
}

void tearDown(void)
{
    nvm_mock_reset();
}

/* ==========================================================================
 * Test suite
 * ========================================================================== */

ZTEST_SUITE(nvm_store_zephyr_ops, NULL, NULL, NULL, NULL, NULL);

/* TC-001 */
ZTEST(nvm_store_zephyr_ops, test_no_ops_registered_reports_not_initialized)
{
    uds_status_t status = nvm_store_init(NULL);

    zassert_equal(UDS_STATUS_ERR_NOT_INITIALIZED, status, "");
    zassert_false(nvm_store_is_ready(), "");
}

/* TC-002 */
ZTEST(nvm_store_zephyr_ops, test_ops_registered_init_succeeds_without_cfg)
{
    fake_ops_register(true);

    zassert_equal(UDS_STATUS_OK, nvm_store_init(NULL), "");
    zassert_true(nvm_store_is_ready(), "");
}

/* TC-003 — "never called" guard: counter, not an assert inside the callback.
 * Baseline is taken AFTER nvm_store_init(), not before: init's own schema
 * check (nvm_check_schema_via_ops(), mirroring
 * platform/freertos/freertos_nvm.c's nvm_check_schema()) already performs
 * one ops.write of its own on first boot — that is correct, intended
 * behaviour, not noise to suppress. */
ZTEST(nvm_store_zephyr_ops, test_write_dispatches_to_registered_ops)
{
    uint8_t  val = 0x42U;
    uint32_t calls_before_write;

    fake_ops_register(true);
    zassert_equal(UDS_STATUS_OK, nvm_store_init(NULL), "");

    calls_before_write = s_fake_write_calls;
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_write(NVM_KEY_LIFECYCLE_CNT, &val, sizeof(val)), "");
    zassert_true(s_fake_write_calls > calls_before_write,
                 "nvm_store_write() must dispatch to the registered "
                 "eds_nvm_ops_t.write — this is the #304 Stage 1 fix "
                 "itself, not incidental to it");
}

/* TC-004 */
ZTEST(nvm_store_zephyr_ops, test_read_dispatches_to_registered_ops)
{
    uint8_t  written  = 0x7AU;
    uint8_t  readback = 0U;
    size_t   read_len = 0U;
    uint32_t calls_before_read;

    fake_ops_register(true);
    zassert_equal(UDS_STATUS_OK, nvm_store_init(NULL), "");
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_write(NVM_KEY_LIFECYCLE_CNT, &written, sizeof(written)), "");

    calls_before_read = s_fake_read_calls;
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_read(NVM_KEY_LIFECYCLE_CNT, &readback, sizeof(readback), &read_len), "");
    zassert_true(s_fake_read_calls > calls_before_read, "");
    zassert_equal(written, readback, "");
    zassert_equal(sizeof(written), read_len, "");
}

/* TC-005 */
ZTEST(nvm_store_zephyr_ops, test_delete_prefers_native_remove)
{
    uint8_t val = 0x01U;

    fake_ops_register(true);
    zassert_equal(UDS_STATUS_OK, nvm_store_init(NULL), "");
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_write(NVM_KEY_LIFECYCLE_CNT, &val, sizeof(val)), "");

    zassert_equal(UDS_STATUS_OK, nvm_store_delete(NVM_KEY_LIFECYCLE_CNT), "");
    zassert_true(s_fake_remove_calls > 0U,
                 "[#287] a backend with a native remove must use it, not "
                 "the sentinel-write fallback");
    zassert_equal(UDS_STATUS_ERR_DID_NOT_FOUND,
                  nvm_store_read(NVM_KEY_LIFECYCLE_CNT, &val, sizeof(val), NULL), "");
}

/* TC-006 — [#285] sentinel fallback + translation, scoped to NVM_KEY_SEC_STATE. */
ZTEST(nvm_store_zephyr_ops, test_delete_falls_back_to_sentinel_without_native_remove)
{
    uint8_t val = 0x03U;

    fake_ops_register(false); /* no .remove */
    zassert_equal(UDS_STATUS_OK, nvm_store_init(NULL), "");
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_write(NVM_KEY_SEC_STATE, &val, sizeof(val)), "");

    zassert_equal(UDS_STATUS_OK, nvm_store_delete(NVM_KEY_SEC_STATE), "");
    zassert_equal(0U, s_fake_remove_calls, "");
    zassert_true(s_fake_write_calls > 1U,
                 "delete must have performed an extra write (the sentinel) "
                 "beyond the original data write above");
    zassert_equal(UDS_STATUS_ERR_DID_NOT_FOUND,
                  nvm_store_read(NVM_KEY_SEC_STATE, &val, sizeof(val), NULL),
                  "[#285] a sentinel-pattern record at NVM_KEY_SEC_STATE "
                  "must read back as absent, not as literal sentinel data");
}

/* TC-007 */
ZTEST(nvm_store_zephyr_ops, test_is_ready_dispatches_to_registered_ops)
{
    fake_ops_register(true);
    zassert_equal(UDS_STATUS_OK, nvm_store_init(NULL), "");

    zassert_equal(0U, s_fake_is_ready_calls, "");
    zassert_true(nvm_store_is_ready(), "");
    zassert_true(s_fake_is_ready_calls > 0U, "");
}

/* TC-008 — [#280] explicit allowlist, mirrors freertos_nvm.c's erase_all()
 * EXACTLY, including its narrower contract for keys other than
 * NVM_KEY_SEC_STATE: erase_all() writes the raw sentinel pattern via
 * s_ops.write() directly (not through nvm_store_delete()), and
 * nvm_store_read()'s sentinel->DID_NOT_FOUND translation is deliberately
 * scoped to NVM_KEY_SEC_STATE only (nvm_store.h's own doc comment on
 * NVM_STORE_DELETE_SENTINEL_LEN/_BYTE). So a non-SEC_STATE key reads back
 * as UDS_STATUS_OK with a 1-byte sentinel record after erase_all() — not
 * DID_NOT_FOUND — exactly like platform/freertos/freertos_nvm.c. */
ZTEST(nvm_store_zephyr_ops, test_erase_all_preserves_sec_state_only)
{
    uint8_t sec_val   = 0x05U;
    uint8_t other_val = 0x09U;
    uint8_t readback;
    size_t  read_len = 0U;

    fake_ops_register(true);
    zassert_equal(UDS_STATUS_OK, nvm_store_init(NULL), "");
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_write(NVM_KEY_SEC_STATE, &sec_val, sizeof(sec_val)), "");
    zassert_equal(UDS_STATUS_OK,
                  nvm_store_write(NVM_KEY_LIFECYCLE_CNT, &other_val, sizeof(other_val)), "");

    zassert_equal(UDS_STATUS_OK, nvm_store_erase_all(), "");

    zassert_equal(UDS_STATUS_OK,
                  nvm_store_read(NVM_KEY_SEC_STATE, &readback, sizeof(readback), NULL), "");
    zassert_equal(sec_val, readback,
                  "[#280] NVM_KEY_SEC_STATE must survive erase_all()");

    zassert_equal(UDS_STATUS_OK,
                  nvm_store_read(NVM_KEY_LIFECYCLE_CNT, &readback, sizeof(readback), &read_len),
                  "every other key is overwritten with the sentinel "
                  "record, not left readable as its old value");
    zassert_equal(NVM_STORE_DELETE_SENTINEL_LEN, read_len, "");
    zassert_equal(NVM_STORE_DELETE_SENTINEL_BYTE, readback, "");
}

/* TC-009 — registering NULL after ops were set, but BEFORE init (which is
 * one-shot: a second nvm_store_init() call would just return
 * UDS_STATUS_ERR_ALREADY_INITIALIZED without re-examining the ops table,
 * which would prove nothing about registration itself). */
ZTEST(nvm_store_zephyr_ops, test_register_null_clears_ops)
{
    fake_ops_register(true);

    zephyr_nvm_store_register_ops(NULL);

    zassert_equal(UDS_STATUS_ERR_NOT_INITIALIZED, nvm_store_init(NULL),
                  "re-registering NULL must clear the ops table back to "
                  "\"not registered\", exactly like "
                  "freertos_nvm_register_ops(NULL)");
}

/* ==========================================================================
 * run_all_tests
 * ========================================================================== */

void run_all_tests(void)
{
    RUN_TEST(nvm_store_zephyr_ops__test_no_ops_registered_reports_not_initialized);
    RUN_TEST(nvm_store_zephyr_ops__test_ops_registered_init_succeeds_without_cfg);
    RUN_TEST(nvm_store_zephyr_ops__test_write_dispatches_to_registered_ops);
    RUN_TEST(nvm_store_zephyr_ops__test_read_dispatches_to_registered_ops);
    RUN_TEST(nvm_store_zephyr_ops__test_delete_prefers_native_remove);
    RUN_TEST(nvm_store_zephyr_ops__test_delete_falls_back_to_sentinel_without_native_remove);
    RUN_TEST(nvm_store_zephyr_ops__test_is_ready_dispatches_to_registered_ops);
    RUN_TEST(nvm_store_zephyr_ops__test_erase_all_preserves_sec_state_only);
    RUN_TEST(nvm_store_zephyr_ops__test_register_null_clears_ops);
}
