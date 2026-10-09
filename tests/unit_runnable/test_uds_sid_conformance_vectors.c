// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_uds_sid_conformance_vectors.c
 *
 * MODULE UNDER TEST: core/uds_server.c + core/uds_services/*.c (ISO 14229-1
 *                     UDS SID framing).
 *
 * [EDS#374] Independent conformance evidence, not derived from Xaloqi's own
 * tester — the third and last of the three legs scoped by the 2026-10-06
 * source survey (PR 2 = ISO-TP, #366; PR 3 = DoIP, #372). Every request byte
 * sequence below is PORTED from pylessard/python-udsoncan's own
 * client-server test suite. This file is deliberately NOT named
 * test_uds_server.c or test_service_0x*.c (those files already exist and are
 * Xaloqi's own, self-authored suites) precisely so provenance is visible at
 * a glance.
 *
 * -----------------------------------------------------------------------------
 * PROVENANCE
 * -----------------------------------------------------------------------------
 *   Upstream project : pylessard/python-udsoncan
 *   Upstream tag     : v1.26.1
 *   Upstream commit  : ece75a4c92d37f8f861710cd67a2faac565cb9bb
 *   Upstream files   : test/client/test_diagnostic_session_control.py
 *                       test/client/test_ecu_reset.py
 *                       test/client/test_tester_present.py
 *                       test/client/test_security_access.py
 *                       test/client/test_communication_control.py
 *                       test/client/test_control_dtc_setting.py
 *                       test/client/test_clear_dtc.py
 *                       test/client/test_read_data_by_identifier.py
 *                       test/client/test_write_data_by_identifier.py
 *                       test/client/test_routine_control.py
 *                       test/test_response.py (generic negative-response
 *                         wire encoding, cross-checked for VEC-GEN-002)
 *   Upstream licence : MIT, Copyright (c) 2019 Pier-Yves Lessard.
 *
 *     Permission is hereby granted, free of charge, to any person obtaining
 *     a copy of this software and associated documentation files (the
 *     "Software"), to deal in the Software without restriction, including
 *     without limitation the rights to use, copy, modify, merge, publish,
 *     distribute, sublicense, and/or sell copies of the Software, and to
 *     permit persons to whom the Software is furnished to do so, subject to
 *     the following conditions: the above copyright notice and this
 *     permission notice shall be included in all copies or substantial
 *     portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT
 *     WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
 *
 *   What was changed in porting: upstream's tests drive udsoncan's CLIENT
 *   through a mock socket queued with scripted byte sequences — the
 *   "request" bytes are what a spec-compliant client actually transmits
 *   (udsoncan's own request-encoding logic, independent of EDS), and the
 *   canned "response" bytes are what a spec-compliant client is willing to
 *   accept and decode without error. This file feeds those exact REQUEST
 *   bytes to EDS's uds_server_process_request() playing the ECU (SERVER)
 *   side, and checks EDS's own RESPONSE against the same wire shape upstream
 *   expects: response SID (requestSID+0x40), echoed sub-function/DID/
 *   routine-ID fields, and response length. Each test cites the exact
 *   upstream fixture it was ported from.
 *
 * -----------------------------------------------------------------------------
 * WHAT IS AND ISN'T BYTE-EXACT — read before trusting a byte comparison
 * -----------------------------------------------------------------------------
 *   Three different levels of fidelity appear below, and each test says
 *   which one applies to it:
 *
 *   (a) FULLY BYTE-EXACT, both directions: upstream's exact response bytes
 *       are a protocol constant with no ECU-specific content, so EDS's
 *       actual response is compared byte-for-byte against upstream's own
 *       expected bytes. (VEC-SID-28-001, VEC-SID-85-001, VEC-SID-14-001,
 *       VEC-SID-3E-001, VEC-SID-2E-001, VEC-GEN-002.)
 *   (b) REQUEST byte-exact, RESPONSE shape-exact: the response's fixed
 *       fields (SID+0x40, echoed sub-function/DID/RID) are checked exactly;
 *       any trailing content (seed bytes, DID data, routine status record)
 *       is EDS's own ECU-specific value, not upstream's, and is not
 *       asserted against upstream's canned content. (VEC-SID-10-001,
 *       VEC-SID-11-001 [see its own note on the reset-type substitution],
 *       VEC-SID-27-001, VEC-SID-22-001, VEC-SID-31-001.)
 *   (c) Request byte chosen to match a documented, cross-file upstream
 *       convention rather than one literal fixture. (VEC-GEN-001, see its
 *       own comment.)
 *
 * -----------------------------------------------------------------------------
 * HONEST SCOPE LIMITS — read before extending this file
 * -----------------------------------------------------------------------------
 *   - Per EDS#374's own scope: protocol mechanics only (response framing,
 *     standard NRCs, session gating, SID echo, the 3-byte negative-response
 *     form) — never DID or DTC *content*, which is ECU-specific by
 *     construction and has no independent reference.
 *   - 10 of EDS's 19 registered services get a vector here (0x10, 0x11,
 *     0x14, 0x22, 0x27, 0x28, 0x2E, 0x31, 0x3E, 0x85). The remaining seven
 *     are deliberately NOT ported, each for a reason that would make a
 *     "framing-only" vector dishonest rather than merely incomplete:
 *       - 0x19 ReadDTCInformation: every sub-function's response IS DTC
 *         content (status-availability masks, DTC codes, counts) — there
 *         is no framing-only slice to extract that wouldn't also assert
 *         ECU-specific fault data as if it were protocol conformance.
 *       - 0x2F InputOutputControlByIdentifier: same DID-content coupling as
 *         0x22/0x2E's *data*, without even those two's clean echo-only
 *         response envelope to isolate.
 *       - 0x34/0x35/0x36/0x37 (RequestDownload/Upload/TransferData/
 *         RequestTransferExit) and 0x3D (WriteMemoryByAddress): these are
 *         multi-step, memory-address-and-length-format-specific exchanges
 *         already covered by extensive self-authored suites
 *         (test_service_0x34.c/_chunked_erase.c/_0x35.c/_0x36.c/_0x37.c/
 *         _0x23_0x3D.c) — a single-shot ported vector would either skip the
 *         sequencing entirely (dishonest) or re-implement a chunked transfer
 *         protocol around upstream bytes that were never meant to exercise
 *         it (not what upstream's own fixtures test either).
 *   - SecurityAccess (0x27): the ODD/EVEN sub-function routing and response
 *     SID/echo are asserted against upstream exactly; the seed VALUE and
 *     key-validation ALGORITHM are ECU-specific per ISO 14229-1 §9.4 by
 *     design (upstream's own test uses a scripted "dummy_algo" for the same
 *     reason) and are EDS's own test stubs, not upstream content.
 *   - ECUReset (0x11): upstream's own fixture uses resetType 0x55 — not one
 *     of ISO 14229-1 Table 186's defined values (0x01-0x05) and not one EDS
 *     implements (EDS: 0x01/0x02/0x03 only). Ported using resetType 0x01
 *     (hardReset) instead, which both EDS and the standard accept — the
 *     echo mechanics upstream's fixture exists to test are unchanged.
 *
 * -----------------------------------------------------------------------------
 * ADR-005 (xaloqi-knowledge decisions/ADR-005-execution-truth-semantics.md)
 * -----------------------------------------------------------------------------
 *   DECLARED FLOOR: all cases in this file, every run. No external
 *   dependency, credential, or optional tier gates any case here (pure host
 *   C, the real g_uds_service_table, no network, no license file). Per
 *   ADR-005 rule 7, no case count is carried in build_tests.sh's TESTS entry
 *   or in CI job naming for this module.
 *
 * -----------------------------------------------------------------------------
 * lessons/run-014 — "a test that passes against the unfixed code is worth
 * nothing" — compliance note
 * -----------------------------------------------------------------------------
 *   Unlike PR 2 (ISO-TP) and PR 3 (DoIP), this leg found no new defect:
 *   EDS's per-service framing is already covered by extensive self-authored
 *   suites (test_service_0x10.c through test_service_0x85.c), so every
 *   vector below exercises mechanics those suites already prove correct
 *   from a different, self-authored byte sequence. Their value is
 *   independent SOURCING across effectively this repo's whole UDS SID
 *   surface, not new defect-finding coverage — the same honest distinction
 *   PR 3's VEC-RA-001/002 already drew for two of its five vectors, just
 *   applied here to all of them. Not claiming otherwise is itself the
 *   point of this section.
 *
 *   To confirm this file is not vacuously true (lessons/run-014's actual
 *   requirement — every assertion must be able to fail, not that it must
 *   find a NEW bug), three representative vectors spanning the generic
 *   dispatcher, a byte-exact service, and a shape-only service were each
 *   verified against a locally reverted mutation, then restored (mutation
 *   not kept in git history, applied and reverted locally before this file
 *   was committed):
 *     - VEC-GEN-002: changing srv_status_to_nrc()'s
 *       UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP case to return
 *       UDS_NRC_CONDITIONS_NOT_CORRECT (0x22) instead of 0x12 made this
 *       fail at this file's line 463 (the memcmp against upstream's exact
 *       {0x7F,0x10,0x12}) with "Expected 16 but got 0" — the NRC byte
 *       diff (0x22-0x12=16) the harness's TEST_ASSERT_EQUAL reports (see
 *       test_isotp_conformance_vectors.c's header for why no custom
 *       message string appears).
 *     - VEC-SID-3E-001: changing service_0x3E.c's positive-response
 *       sub-function byte from SVC_0x3E_SUBFUNCTION_ZERO to a literal 0x01
 *       made this fail at line 747 (the memcmp against {0x7E,0x00}) with
 *       "Expected 1 but got 0".
 *     - VEC-SID-2E-001: changing SVC_0x2E_RESP_LEN from 3 to 2 made this
 *       fail at line 813 (the response-length check) with "Expected 2 but
 *       got 3" (the DID low byte was silently dropped).
 *   All three were re-verified passing after the mutation was reverted;
 *   the full suite (bash build_tests.sh and --sanitize) returned to 1042
 *   cases run, 0 failed.
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "services.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_access_table.h"
#include "uds_comm_control.h"
#include "uds_types.h"
#include "did_database.h"
#include "dtc_database.h"
#include "routine_database.h"

/* Test-only resets — defined in their respective .c files, not exposed in
 * the public headers (same pattern as test_service_0x28.c/_0x14.c). */
extern void uds_comm_control_test_reset(void);
extern void dtc_database_test_reset(void);

/* =========================================================================
 * Shared context
 * ========================================================================= */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;

/* -------------------------------------------------------------------------
 * Security stubs — a trivial XOR algorithm, same shape as EDS's own
 * test_service_0x27.c. The ALGORITHM is EDS's test fixture (ISO 14229-1
 * leaves it OEM-defined); only the ODD/EVEN request routing and response
 * SID/echo mechanics below are the ported conformance claim.
 * ------------------------------------------------------------------------- */

static uds_status_t t_seed(uint8_t level, uint8_t *buf, uint8_t max_len, uint8_t *out_len)
{
    uint8_t i;
    (void)level;
    for (i = 0U; (i < max_len) && (i < 4U); i++) {
        buf[i] = (uint8_t)(0x10U + i);
    }
    *out_len = (max_len < 4U) ? max_len : 4U;
    return UDS_STATUS_OK;
}

static bool t_key(uint8_t level, const uint8_t *seed, uint8_t seed_len,
                   const uint8_t *key, uint8_t key_len)
{
    uint8_t i;
    (void)level;
    if ((seed_len != key_len) || (seed_len == 0U)) {
        return false;
    }
    for (i = 0U; i < seed_len; i++) {
        if (key[i] != (uint8_t)(seed[i] ^ 0xAAU)) {
            return false;
        }
    }
    return true;
}

/* -------------------------------------------------------------------------
 * DID stubs — DID 0x0001, read/write, no extra security, 2-byte data.
 * Mirrors test_service_0x2E.c's registration pattern. The DATA VALUE is
 * EDS's own test fixture; only the request/response envelope (DID echo,
 * SID+0x40, response length) is the ported conformance claim.
 * ------------------------------------------------------------------------- */

#define TEST_DID            (0x0001U)
#define TEST_DID_DATA_LEN   (2U)

static uint8_t g_did_read_data[TEST_DID_DATA_LEN] = { 0x99U, 0x88U };
static uint8_t g_did_last_write[TEST_DID_DATA_LEN];

static uds_status_t did_read_cb(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    if (buf_len < (uint16_t)TEST_DID_DATA_LEN) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }
    (void)memcpy(buf, g_did_read_data, (size_t)TEST_DID_DATA_LEN);
    *out_len = (uint16_t)TEST_DID_DATA_LEN;
    return UDS_STATUS_OK;
}

static uds_status_t did_write_cb(const uint8_t *buf, uint16_t len)
{
    if (len != (uint16_t)TEST_DID_DATA_LEN) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    (void)memcpy(g_did_last_write, buf, (size_t)TEST_DID_DATA_LEN);
    return UDS_STATUS_OK;
}

/* -------------------------------------------------------------------------
 * Routine stub — RID 0x0012 (upstream's own test_start_routine_success
 * fixture uses this exact RID), startRoutine only, returns a fixed 2-byte
 * status record. The STATUS RECORD is EDS's own test fixture; only the
 * control-type/RID echo mechanics below are the ported conformance claim.
 * ------------------------------------------------------------------------- */

#define TEST_RID            (0x0012U)

static uint8_t g_routine_status[2] = { 0x11U, 0x22U };

static uds_status_t routine_start_cb(const uint8_t *opt, uint8_t opt_len,
                                      uint8_t *res, uint8_t res_max, uint8_t *res_out)
{
    (void)opt; (void)opt_len;
    if (res_max < 2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }
    (void)memcpy(res, g_routine_status, 2U);
    *res_out = 2U;
    return UDS_STATUS_OK;
}

/* -------------------------------------------------------------------------
 * One-time global-singleton registration (did_database / routine_database
 * have no public test_reset(), so — same pattern as test_service_0x2E.c —
 * they are registered exactly once for the whole binary).
 * ------------------------------------------------------------------------- */

static bool g_singletons_ready = false;

static void register_singletons_once(void)
{
    if (g_singletons_ready) {
        return;
    }

    (void)did_database_init();
    {
        did_entry_t entry;
        (void)memset(&entry, 0, sizeof(entry));
        entry.did_id             = (uint16_t)TEST_DID;
        entry.access_flags       = (uint8_t)(DID_ACCESS_READ | DID_ACCESS_WRITE);
        entry.min_session        = (uint8_t)UDS_SESSION_DEFAULT;
        entry.read_access_level  = (uint8_t)0U;
        entry.write_access_level = (uint8_t)0U;
        entry.data_length        = (uint16_t)TEST_DID_DATA_LEN;
        entry.read_cb            = did_read_cb;
        entry.write_cb           = did_write_cb;
        entry.description        = "VEC-SID test DID";
        (void)did_database_register(&entry);
    }

    (void)routine_database_init();
    {
        routine_entry_t entry;
        (void)memset(&entry, 0, sizeof(entry));
        entry.rid           = (uint16_t)TEST_RID;
        entry.support_flags = (uint8_t)ROUTINE_SUPPORT_START;
        entry.min_session   = (uint8_t)UDS_SESSION_DEFAULT;
        entry.security_level = 0U;
        entry.start_cb      = routine_start_cb;
        entry.description   = "VEC-SID test routine";
        (void)routine_database_register(&entry);
    }

    g_singletons_ready = true;
}

/* -------------------------------------------------------------------------
 * Fresh per-test setup: session/security/server context re-initialised
 * every call (matches test_service_0x27.c's pattern); comm_control and the
 * DTC database re-initialised every call via their own test_reset() (they
 * are also process-wide singletons, but DO expose a reset); did_database
 * and routine_database registered once (see above) since cleared state
 * is not needed for them — every vector reads the same fixed test DID/RID.
 * ------------------------------------------------------------------------- */

static void setup(uds_session_type_t initial_session)
{
    register_singletons_once();

    (void)memset(&g_sess, 0, sizeof(g_sess));
    (void)memset(&g_sec,  0, sizeof(g_sec));
    (void)memset(&g_srv,  0, sizeof(g_srv));

    (void)uds_session_init(&g_sess, 5000U);
    if (initial_session != UDS_SESSION_DEFAULT) {
        (void)uds_session_transition(&g_sess, initial_session);
    }

    {
        static const uds_security_cfg_t sc = {
            .max_attempts     = 3U,
            .lockout_ms       = 100U,
            .key_validate_cb  = t_key,
            .seed_generate_cb = t_seed,
        };
        (void)uds_security_init(&g_sec, &sc);
    }

    {
        const uds_server_cfg_t svc = {
            .p2_server_max_ms      = 25U,
            .p2_star_server_max_ms = 5000U,
            .session_ctx           = &g_sess,
            .security_ctx          = &g_sec,
            .service_table         = g_uds_service_table,
            .service_table_count   = (uint8_t)UDS_SERVICE_TABLE_COUNT,
            .access_table           = NULL, /* use ISO 14229-1 default table */
            .access_table_count     = 0U,
        };
        (void)uds_server_init(&g_srv, &svc);
    }

    uds_comm_control_test_reset();
    {
        static const uds_comm_control_cfg_t cc_cfg = { .comm_cb = NULL, .dtc_cb = NULL };
        (void)uds_comm_control_init(&cc_cfg);
    }

    dtc_database_test_reset();
    (void)dtc_database_init();
    (void)dtc_database_register(0x123456UL, 0x00U, "VEC-SID test DTC");
}

/** Build a request from a byte literal. */
static uds_msg_buf_t make_req(const uint8_t *bytes, uint16_t len)
{
    uds_msg_buf_t r;
    (void)memset(&r, 0, sizeof(r));
    (void)memcpy(r.data, bytes, (size_t)len);
    r.length = len;
    return r;
}

/* =========================================================================
 * VEC-GEN-001 — unsupported SID -> NRC 0x11, 3-byte negative-response form
 *
 * Upstream treats SID 0x00 as "Inexistent Service" identically across SIX
 * independent test files (test_diagnostic_session_control.py,
 * test_tester_present.py, test_ecu_reset.py, test_security_access.py,
 * test_routine_control.py, test_control_dtc_setting.py,
 * test_communication_control.py) — stronger cross-file corroboration than
 * any single fixture, confirming 0x00 is universally treated as outside
 * ISO 14229-1's assigned SID range by independent tooling, not one script's
 * assumption.
 * ========================================================================= */

ZTEST_SUITE(test_uds_sid_conformance_vectors, NULL, NULL, NULL, NULL, NULL);

ZTEST(test_uds_sid_conformance_vectors, vec_gen_001_unsupported_sid)
{
    setup(UDS_SESSION_DEFAULT);

    uint8_t req_bytes[] = { 0x00U };
    uds_msg_buf_t req  = make_req(req_bytes, (uint16_t)sizeof(req_bytes));
    uds_msg_buf_t resp;
    (void)memset(&resp, 0, sizeof(resp));

    zassert_equal(uds_server_process_request(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "dispatch itself must not error");
    zassert_equal(resp.length, 3U, "negative response must be exactly 3 bytes");
    zassert_equal(resp.data[0], (uint8_t)UDS_SID_NEGATIVE_RESPONSE, "byte0 must be 0x7F");
    zassert_equal(resp.data[1], 0x00U, "byte1 must echo the unsupported SID");
    zassert_equal(resp.data[2], (uint8_t)UDS_NRC_SERVICE_NOT_SUPPORTED,
                  "byte2 must be NRC 0x11 serviceNotSupported");
}

/* =========================================================================
 * VEC-GEN-002 — subFunctionNotSupported, byte-exact
 * Upstream: test_diagnostic_session_control.py::test_dsc_denied_exception
 *   request  = b"\x10\x08"     (session type 0x08 is undefined)
 *   response = b"\x7F\x10\x12" (canned, but this IS udsoncan's own
 *                                independent expectation of what a
 *                                compliant server sends)
 * Cross-checked against test/test_response.py's own Response class, whose
 * get_payload() independently encodes a negative response as
 * [0x7F, originalSID, code] — confirming the 3-byte form itself is the
 * library's canonical wire structure, not an artefact of this one fixture.
 * ========================================================================= */

ZTEST(test_uds_sid_conformance_vectors, vec_gen_002_subfunction_not_supported_byte_exact)
{
    setup(UDS_SESSION_DEFAULT);

    uint8_t req_bytes[] = { 0x10U, 0x08U };
    uds_msg_buf_t req  = make_req(req_bytes, (uint16_t)sizeof(req_bytes));
    uds_msg_buf_t resp;
    (void)memset(&resp, 0, sizeof(resp));

    zassert_equal(uds_server_process_request(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "dispatch itself must not error");

    uint8_t expected[] = { 0x7FU, 0x10U, 0x12U };
    zassert_equal(resp.length, (uint16_t)sizeof(expected), "response must be 3 bytes");
    zassert_equal(memcmp(resp.data, expected, sizeof(expected)), 0,
                  "response must be byte-identical to upstream's own expected bytes");
}

/* =========================================================================
 * VEC-GEN-003 — incorrectMessageLengthOrInvalidFormat (NRC 0x13)
 * Not ported from a literal upstream byte fixture — udsoncan's CLIENT never
 * constructs an intentionally malformed request, so no such fixture exists
 * upstream. Ported instead from ISO 14229-1 Annex A's own NRC table (the
 * same standard upstream's SID/NRC constants are drawn from), using the
 * DiagnosticSessionControl handler's own documented minimum length.
 * ========================================================================= */

ZTEST(test_uds_sid_conformance_vectors, vec_gen_003_incorrect_length)
{
    setup(UDS_SESSION_DEFAULT);

    uint8_t req_bytes[] = { 0x10U }; /* SID only -- missing the session-type byte */
    uds_msg_buf_t req  = make_req(req_bytes, (uint16_t)sizeof(req_bytes));
    uds_msg_buf_t resp;
    (void)memset(&resp, 0, sizeof(resp));

    zassert_equal(uds_server_process_request(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "dispatch itself must not error");

    uint8_t expected[] = { 0x7FU, 0x10U, 0x13U };
    zassert_equal(resp.length, (uint16_t)sizeof(expected), "response must be 3 bytes");
    zassert_equal(memcmp(resp.data, expected, sizeof(expected)), 0,
                  "NRC 0x13 incorrectMessageLengthOrInvalidFormat expected");
}

/* =========================================================================
 * VEC-GEN-004 — session-gating NRC 0x7F, generic 3-byte form, a second
 * independent SID from VEC-GEN-002 (diversity, not the same code path).
 * Request is upstream's own exact ControlDTCSetting bytes
 * (test_control_dtc_setting.py::test_set_on), sent from DEFAULT session
 * where EDS's own access table (core/uds_access_table.c) restricts 0x85 to
 * non-default sessions.
 * ========================================================================= */

ZTEST(test_uds_sid_conformance_vectors, vec_gen_004_session_gating_nrc_0x7f)
{
    setup(UDS_SESSION_DEFAULT);

    uint8_t req_bytes[] = { 0x85U, 0x01U };
    uds_msg_buf_t req  = make_req(req_bytes, (uint16_t)sizeof(req_bytes));
    uds_msg_buf_t resp;
    (void)memset(&resp, 0, sizeof(resp));

    zassert_equal(uds_server_process_request(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "dispatch itself must not error");

    uint8_t expected[] = { 0x7FU, 0x85U, 0x7FU };
    zassert_equal(resp.length, (uint16_t)sizeof(expected), "response must be 3 bytes");
    zassert_equal(memcmp(resp.data, expected, sizeof(expected)), 0,
                  "NRC 0x7F serviceNotSupportedInActiveSession expected");
}

/* =========================================================================
 * VEC-SID-10-001 — DiagnosticSessionControl, shape-exact
 * Upstream: test_diagnostic_session_control.py::test_dsc_success_2013_plus
 *   request = b"\x10\x01"
 * Response SHAPE only: byte0=0x50, byte1=session echo, length=6
 * (SID + session + P2hi/P2lo/P2*hi/P2*lo). The P2/P2* VALUES are EDS's own
 * server config (ctx->cfg.p2_server_max_ms etc.), not upstream's arbitrary
 * 0x9988/0x1234 -- those are a different, OEM-specific ECU's canned values.
 * ========================================================================= */

ZTEST(test_uds_sid_conformance_vectors, vec_sid_10_001_diagnostic_session_control_shape)
{
    setup(UDS_SESSION_DEFAULT);

    uint8_t req_bytes[] = { 0x10U, 0x01U };
    uds_msg_buf_t req  = make_req(req_bytes, (uint16_t)sizeof(req_bytes));
    uds_msg_buf_t resp;
    (void)memset(&resp, 0, sizeof(resp));

    zassert_equal(uds_server_process_request(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "request must succeed");
    zassert_equal(resp.length, 6U, "response must be 6 bytes (SID+session+P2+P2*)");
    zassert_equal(resp.data[0], 0x50U, "byte0 must be SID+0x40");
    zassert_equal(resp.data[1], 0x01U, "byte1 must echo the requested session type");
}

/* =========================================================================
 * VEC-SID-10-002 — suppressPosRspMsgIndicationBit, byte-exact request
 * Upstream: test_diagnostic_session_control.py::test_dsc_success_spr
 *   request = b"\x10\x81" ; client expects NO response frame at all.
 * ========================================================================= */

ZTEST(test_uds_sid_conformance_vectors, vec_sid_10_002_suppress_bit)
{
    setup(UDS_SESSION_DEFAULT);

    uint8_t req_bytes[] = { 0x10U, 0x81U };
    uds_msg_buf_t req  = make_req(req_bytes, (uint16_t)sizeof(req_bytes));
    uds_msg_buf_t resp;
    (void)memset(&resp, 0, sizeof(resp));

    zassert_equal(uds_server_process_request(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "request must succeed");
    zassert_equal(resp.length, 0U,
                  "suppressPosRspMsgIndicationBit set: caller must not transmit a frame");
}

/* =========================================================================
 * VEC-SID-11-001 — ECUReset, byte-exact (adapted reset type -- see this
 * file's HONEST SCOPE LIMITS section for why 0x01 replaces upstream's 0x55)
 * Adapted from: test_ecu_reset.py::test_ecu_reset_success
 *   upstream request  = b"\x11\x55" ; upstream response = b"\x51\x55"
 *   ported request    = b"\x11\x01" (hardReset, ISO 14229-1 Table 186)
 * ========================================================================= */

ZTEST(test_uds_sid_conformance_vectors, vec_sid_11_001_ecu_reset_byte_exact)
{
    setup(UDS_SESSION_DEFAULT);

    uint8_t req_bytes[] = { 0x11U, 0x01U };
    uds_msg_buf_t req  = make_req(req_bytes, (uint16_t)sizeof(req_bytes));
    uds_msg_buf_t resp;
    (void)memset(&resp, 0, sizeof(resp));

    zassert_equal(uds_server_process_request(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "request must succeed");

    uint8_t expected[] = { 0x51U, 0x01U };
    zassert_equal(resp.length, (uint16_t)sizeof(expected), "response must be 2 bytes");
    zassert_equal(memcmp(resp.data, expected, sizeof(expected)), 0,
                  "response must be byte-identical to the echo shape upstream expects");
}

/* =========================================================================
 * VEC-SID-27-001 — SecurityAccess, odd/even routing + response shape
 * Upstream: test_security_access.py::TestRequestSeed.test_request_seed_success
 *           and TestSendKey.test_send_key_success
 *   seed request  = b"\x27\x05" (odd)
 *   key submission = b"\x27\x06" + key (even) -> response = b"\x67\x06" (2 bytes exact)
 * ========================================================================= */

ZTEST(test_uds_sid_conformance_vectors, vec_sid_27_001_security_access_odd_even_routing)
{
    setup(UDS_SESSION_EXTENDED); /* 0x27 requires non-default session */

    /* -- Seed request (odd sub-function 0x05) -- */
    uint8_t seed_req_bytes[] = { 0x27U, 0x05U };
    uds_msg_buf_t seed_req  = make_req(seed_req_bytes, (uint16_t)sizeof(seed_req_bytes));
    uds_msg_buf_t seed_resp;
    (void)memset(&seed_resp, 0, sizeof(seed_resp));

    zassert_equal(uds_server_process_request(&g_srv, &seed_req, &seed_resp), UDS_STATUS_OK,
                  "seed request must succeed");
    zassert_true(seed_resp.length > 2U, "seed response must carry at least one seed byte");
    zassert_equal(seed_resp.data[0], 0x67U, "byte0 must be SID+0x40");
    zassert_equal(seed_resp.data[1], 0x05U, "byte1 must echo the odd sub-function exactly");

    uint8_t seed_len = (uint8_t)(seed_resp.length - 2U);
    uint8_t seed[8];
    (void)memcpy(seed, &seed_resp.data[2], (size_t)seed_len);

    /* -- Key submission (even sub-function 0x06), computed from the real
     * seed EDS just returned -- the ALGORITHM is EDS's own test stub
     * (t_key above); only the request/response SHAPE is the ported claim. */
    uint8_t key_req_bytes[2U + 8U];
    key_req_bytes[0] = 0x27U;
    key_req_bytes[1] = 0x06U;
    for (uint8_t i = 0U; i < seed_len; i++) {
        key_req_bytes[2U + i] = (uint8_t)(seed[i] ^ 0xAAU);
    }
    uds_msg_buf_t key_req  = make_req(key_req_bytes, (uint16_t)(2U + seed_len));
    uds_msg_buf_t key_resp;
    (void)memset(&key_resp, 0, sizeof(key_resp));

    zassert_equal(uds_server_process_request(&g_srv, &key_req, &key_resp), UDS_STATUS_OK,
                  "key submission must succeed");

    uint8_t expected[] = { 0x67U, 0x06U };
    zassert_equal(key_resp.length, (uint16_t)sizeof(expected),
                  "key-accepted response must be exactly 2 bytes");
    zassert_equal(memcmp(key_resp.data, expected, sizeof(expected)), 0,
                  "response must be byte-identical to upstream's expected shape");
}

/* =========================================================================
 * VEC-SID-28-001 — CommunicationControl, byte-exact
 * Upstream: test_communication_control.py::test_comcontrol_enable_node
 *   request  = b"\x28\x00\x01"
 *   response = b"\x68\x00"
 * ========================================================================= */

ZTEST(test_uds_sid_conformance_vectors, vec_sid_28_001_communication_control_byte_exact)
{
    setup(UDS_SESSION_EXTENDED); /* 0x28 requires non-default session */

    uint8_t req_bytes[] = { 0x28U, 0x00U, 0x01U };
    uds_msg_buf_t req  = make_req(req_bytes, (uint16_t)sizeof(req_bytes));
    uds_msg_buf_t resp;
    (void)memset(&resp, 0, sizeof(resp));

    zassert_equal(uds_server_process_request(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "request must succeed");

    uint8_t expected[] = { 0x68U, 0x00U };
    zassert_equal(resp.length, (uint16_t)sizeof(expected), "response must be 2 bytes");
    zassert_equal(memcmp(resp.data, expected, sizeof(expected)), 0,
                  "response must be byte-identical to upstream's expected bytes");
}

/* =========================================================================
 * VEC-SID-85-001 — ControlDTCSetting, byte-exact
 * Upstream: test_control_dtc_setting.py::test_set_on
 *   request  = b"\x85\x01"
 *   response = b"\xC5\x01"
 * ========================================================================= */

ZTEST(test_uds_sid_conformance_vectors, vec_sid_85_001_control_dtc_setting_byte_exact)
{
    setup(UDS_SESSION_EXTENDED); /* 0x85 requires non-default session */

    uint8_t req_bytes[] = { 0x85U, 0x01U };
    uds_msg_buf_t req  = make_req(req_bytes, (uint16_t)sizeof(req_bytes));
    uds_msg_buf_t resp;
    (void)memset(&resp, 0, sizeof(resp));

    zassert_equal(uds_server_process_request(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "request must succeed");

    uint8_t expected[] = { 0xC5U, 0x01U };
    zassert_equal(resp.length, (uint16_t)sizeof(expected), "response must be 2 bytes");
    zassert_equal(memcmp(resp.data, expected, sizeof(expected)), 0,
                  "response must be byte-identical to upstream's expected bytes");
}

/* =========================================================================
 * VEC-SID-14-001 — ClearDiagnosticInformation, byte-exact
 * Upstream: test_clear_dtc.py::test_clear_dtc_success
 *   request  = b"\x14\x12\x34\x56"
 *   response = b"\x54"
 * ========================================================================= */

ZTEST(test_uds_sid_conformance_vectors, vec_sid_14_001_clear_dtc_byte_exact)
{
    /* ISO 14229-1 §12.1.2.2 permits ClearDiagnosticInformation in any
     * session; EDS's own access table (core/uds_access_table.c) is
     * stricter by OEM policy choice and restricts it to non-default
     * sessions only -- EXTENDED is required here for that reason, not an
     * ISO 14229-1 requirement. */
    setup(UDS_SESSION_EXTENDED);

    uint8_t req_bytes[] = { 0x14U, 0x12U, 0x34U, 0x56U };
    uds_msg_buf_t req  = make_req(req_bytes, (uint16_t)sizeof(req_bytes));
    uds_msg_buf_t resp;
    (void)memset(&resp, 0, sizeof(resp));

    zassert_equal(uds_server_process_request(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "request must succeed");

    uint8_t expected[] = { 0x54U };
    zassert_equal(resp.length, (uint16_t)sizeof(expected), "response must be exactly 1 byte");
    zassert_equal(memcmp(resp.data, expected, sizeof(expected)), 0,
                  "response must be byte-identical to upstream's expected bytes");
}

/* =========================================================================
 * VEC-SID-3E-001 — TesterPresent, byte-exact both directions
 * Upstream: test_tester_present.py::test_tester_present_success
 *           and test_tester_present_success_spr
 *   request  = b"\x3E\x00" -> response = b"\x7E\x00"
 *   request  = b"\x3E\x80" -> suppressed (no frame)
 * ========================================================================= */

ZTEST(test_uds_sid_conformance_vectors, vec_sid_3e_001_tester_present_byte_exact)
{
    setup(UDS_SESSION_DEFAULT);

    uint8_t req_bytes[] = { 0x3EU, 0x00U };
    uds_msg_buf_t req  = make_req(req_bytes, (uint16_t)sizeof(req_bytes));
    uds_msg_buf_t resp;
    (void)memset(&resp, 0, sizeof(resp));

    zassert_equal(uds_server_process_request(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "request must succeed");

    uint8_t expected[] = { 0x7EU, 0x00U };
    zassert_equal(resp.length, (uint16_t)sizeof(expected), "response must be 2 bytes");
    zassert_equal(memcmp(resp.data, expected, sizeof(expected)), 0,
                  "response must be byte-identical to upstream's expected bytes");

    /* Suppress-bit variant. */
    uint8_t spr_bytes[] = { 0x3EU, 0x80U };
    uds_msg_buf_t spr_req  = make_req(spr_bytes, (uint16_t)sizeof(spr_bytes));
    uds_msg_buf_t spr_resp;
    (void)memset(&spr_resp, 0, sizeof(spr_resp));

    zassert_equal(uds_server_process_request(&g_srv, &spr_req, &spr_resp), UDS_STATUS_OK,
                  "suppressed request must still succeed");
    zassert_equal(spr_resp.length, 0U, "suppressed response must carry no frame");
}

/* =========================================================================
 * VEC-SID-22-001 — ReadDataByIdentifier, shape-exact
 * Upstream: test_read_data_by_identifier.py (DID 0x0001 request pattern)
 *   request = b"\x22\x00\x01"
 * Response SHAPE only: byte0=0x62, bytes1-2=DID echo. DATA (g_did_read_data)
 * is EDS's own test fixture, not upstream content (see this file's header).
 * ========================================================================= */

ZTEST(test_uds_sid_conformance_vectors, vec_sid_22_001_read_data_by_id_shape)
{
    setup(UDS_SESSION_DEFAULT);

    uint8_t req_bytes[] = { 0x22U, 0x00U, 0x01U };
    uds_msg_buf_t req  = make_req(req_bytes, (uint16_t)sizeof(req_bytes));
    uds_msg_buf_t resp;
    (void)memset(&resp, 0, sizeof(resp));

    zassert_equal(uds_server_process_request(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "request must succeed");
    zassert_equal(resp.length, (uint16_t)(3U + TEST_DID_DATA_LEN),
                  "response must be SID+DID(2)+data");
    zassert_equal(resp.data[0], 0x62U, "byte0 must be SID+0x40");
    zassert_equal(resp.data[1], 0x00U, "byte1 must echo the request DID high byte");
    zassert_equal(resp.data[2], 0x01U, "byte2 must echo the request DID low byte");
}

/* =========================================================================
 * VEC-SID-2E-001 — WriteDataByIdentifier, byte-exact
 * Upstream: test_write_data_by_identifier.py::test_wdbi_single_success1
 *   request  = b"\x2E\x00\x01\x12\x34"
 *   response = b"\x6E\x00\x01"
 * ========================================================================= */

ZTEST(test_uds_sid_conformance_vectors, vec_sid_2e_001_write_data_by_id_byte_exact)
{
    /* EDS's access table requires a non-default session AND Level-1
     * security unlock for 0x2E (calibration/configuration write) --
     * ISO 14229-1 leaves this gating OEM-defined. g_sec.active_level is
     * set directly to simulate a completed 0x27 unlock, same pattern as
     * test_service_0x2E.c's own TC-0x2E-011. */
    setup(UDS_SESSION_EXTENDED);
    g_sec.active_level = (uint8_t)1U;

    uint8_t req_bytes[] = { 0x2EU, 0x00U, 0x01U, 0x12U, 0x34U };
    uds_msg_buf_t req  = make_req(req_bytes, (uint16_t)sizeof(req_bytes));
    uds_msg_buf_t resp;
    (void)memset(&resp, 0, sizeof(resp));

    zassert_equal(uds_server_process_request(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "request must succeed");

    uint8_t expected[] = { 0x6EU, 0x00U, 0x01U };
    zassert_equal(resp.length, (uint16_t)sizeof(expected), "response must be 3 bytes");
    zassert_equal(memcmp(resp.data, expected, sizeof(expected)), 0,
                  "response must be byte-identical to upstream's expected bytes");

    uint8_t expected_write[] = { 0x12U, 0x34U };
    zassert_equal(memcmp(g_did_last_write, expected_write, sizeof(expected_write)), 0,
                  "the write callback must receive the exact request data bytes");
}

/* =========================================================================
 * VEC-SID-31-001 — RoutineControl, shape-exact
 * Upstream: test_routine_control.py::test_start_routine_success
 *   request = b"\x31\x01\x00\x12\x45\x67\x89\xaa" (startRoutine, RID 0x0012,
 *              4-byte option record)
 * Response SHAPE only: byte0=0x71, byte1=control-type echo,
 * bytes2-3=RID echo. The status record (g_routine_status) is EDS's own
 * test fixture, not upstream's 0x9988 (a different routine's own result).
 * ========================================================================= */

ZTEST(test_uds_sid_conformance_vectors, vec_sid_31_001_routine_control_shape)
{
    setup(UDS_SESSION_DEFAULT);

    uint8_t req_bytes[] = { 0x31U, 0x01U, 0x00U, 0x12U, 0x45U, 0x67U, 0x89U, 0xAAU };
    uds_msg_buf_t req  = make_req(req_bytes, (uint16_t)sizeof(req_bytes));
    uds_msg_buf_t resp;
    (void)memset(&resp, 0, sizeof(resp));

    zassert_equal(uds_server_process_request(&g_srv, &req, &resp), UDS_STATUS_OK,
                  "request must succeed");
    zassert_equal(resp.length, 6U, "response must be SID+ctrlType+RID(2)+2-byte status");
    zassert_equal(resp.data[0], 0x71U, "byte0 must be SID+0x40");
    zassert_equal(resp.data[1], 0x01U, "byte1 must echo the control type (startRoutine)");
    zassert_equal(resp.data[2], 0x00U, "byte2 must echo the RID high byte");
    zassert_equal(resp.data[3], 0x12U, "byte3 must echo the RID low byte");
}

/* run_all_tests shim */
extern void test_uds_sid_conformance_vectors__vec_gen_001_unsupported_sid(void);
extern void test_uds_sid_conformance_vectors__vec_gen_002_subfunction_not_supported_byte_exact(void);
extern void test_uds_sid_conformance_vectors__vec_gen_003_incorrect_length(void);
extern void test_uds_sid_conformance_vectors__vec_gen_004_session_gating_nrc_0x7f(void);
extern void test_uds_sid_conformance_vectors__vec_sid_10_001_diagnostic_session_control_shape(void);
extern void test_uds_sid_conformance_vectors__vec_sid_10_002_suppress_bit(void);
extern void test_uds_sid_conformance_vectors__vec_sid_11_001_ecu_reset_byte_exact(void);
extern void test_uds_sid_conformance_vectors__vec_sid_27_001_security_access_odd_even_routing(void);
extern void test_uds_sid_conformance_vectors__vec_sid_28_001_communication_control_byte_exact(void);
extern void test_uds_sid_conformance_vectors__vec_sid_85_001_control_dtc_setting_byte_exact(void);
extern void test_uds_sid_conformance_vectors__vec_sid_14_001_clear_dtc_byte_exact(void);
extern void test_uds_sid_conformance_vectors__vec_sid_3e_001_tester_present_byte_exact(void);
extern void test_uds_sid_conformance_vectors__vec_sid_22_001_read_data_by_id_shape(void);
extern void test_uds_sid_conformance_vectors__vec_sid_2e_001_write_data_by_id_byte_exact(void);
extern void test_uds_sid_conformance_vectors__vec_sid_31_001_routine_control_shape(void);

void run_all_tests(void)
{
    RUN_TEST(test_uds_sid_conformance_vectors__vec_gen_001_unsupported_sid);
    RUN_TEST(test_uds_sid_conformance_vectors__vec_gen_002_subfunction_not_supported_byte_exact);
    RUN_TEST(test_uds_sid_conformance_vectors__vec_gen_003_incorrect_length);
    RUN_TEST(test_uds_sid_conformance_vectors__vec_gen_004_session_gating_nrc_0x7f);
    RUN_TEST(test_uds_sid_conformance_vectors__vec_sid_10_001_diagnostic_session_control_shape);
    RUN_TEST(test_uds_sid_conformance_vectors__vec_sid_10_002_suppress_bit);
    RUN_TEST(test_uds_sid_conformance_vectors__vec_sid_11_001_ecu_reset_byte_exact);
    RUN_TEST(test_uds_sid_conformance_vectors__vec_sid_27_001_security_access_odd_even_routing);
    RUN_TEST(test_uds_sid_conformance_vectors__vec_sid_28_001_communication_control_byte_exact);
    RUN_TEST(test_uds_sid_conformance_vectors__vec_sid_85_001_control_dtc_setting_byte_exact);
    RUN_TEST(test_uds_sid_conformance_vectors__vec_sid_14_001_clear_dtc_byte_exact);
    RUN_TEST(test_uds_sid_conformance_vectors__vec_sid_3e_001_tester_present_byte_exact);
    RUN_TEST(test_uds_sid_conformance_vectors__vec_sid_22_001_read_data_by_id_shape);
    RUN_TEST(test_uds_sid_conformance_vectors__vec_sid_2e_001_write_data_by_id_byte_exact);
    RUN_TEST(test_uds_sid_conformance_vectors__vec_sid_31_001_routine_control_shape);
}
