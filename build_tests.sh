#!/usr/bin/env bash
# =============================================================================
# Xaloqi EDS
# build_tests.sh  (project root)
#
# PURPOSE: Build and run all 43 host-side C unit tests.
#          Each test module is compiled independently with the full
#          diagnostics stack linked in, then executed immediately.
#
# CANONICAL TEST DIRECTORY: tests/unit_runnable/
#   This script and tests/CMakeLists.txt both reference tests/unit_runnable/.
#   [M2 FIX] CMakeLists.txt was previously pointing at tests/unit/ (which
#   does not exist). Fixed in v0.5.0 — both build paths now agree.
#
# FIX (Technical Review — Issue A1):
#   This file was absent from the repository. The CI 'unit-tests' job runs
#   "bash build_tests.sh" as its primary step; without this file every CI
#   run failed with "build_tests.sh: No such file or directory".
#
# PREREQUISITES:
#   gcc >= 9, python3, pyyaml, jinja2
#   Run codegen first:
#     python3 tools/codegen.py \
#         --config examples/basic_ecu/diagnostics_config.yaml \
#         --out examples/basic_ecu/generated/ --safety-wrappers --asil-level B --no-manifest
#
# USAGE:
#   bash build_tests.sh              # Build + run all 43 tests
#   bash build_tests.sh --verbose    # Show individual test output
#   bash build_tests.sh --keep-bin   # Keep binaries in build_test_host/
#   bash build_tests.sh --coverage   # Build with gcov + generate lcov HTML report
#
# [P2-4] COVERAGE:
#   Pass --coverage to compile with -fprofile-arcs -ftest-coverage,
#   run all tests, collect .gcda data, then call lcov + genhtml to
#   produce build_test_host/coverage/index.html.
#
#   Safety-critical module minimum thresholds (enforced by --coverage):
#     core/uds_safety.c         >= 95% line coverage
#     generated/did_safety_*    >= 95% line coverage
#     config/dtc_mirror.c       >= 90% line coverage
#     All other modules         >= 80% line coverage
#
# EXIT CODES:
#   0  All tests passed.
#   1  One or more tests failed or failed to build.
#
# OUTPUT:
#   Prints one line per test: PASS / FAIL / BUILD_FAIL
#   Final summary line: "=== Unit Test Summary: N passed, M failed ==="
#
# SAFETY CONTEXT: All modules are compiled with -DUNIT_TEST=1 and
#   -DNVM_STORE_HOST_MOCK=1.  These flags activate test-only resets and
#   substitute a malloc-backed NVM store for the Zephyr flash driver.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${SCRIPT_DIR}"
BUILD_DIR="${ROOT}/build_test_host"
VERBOSE=0
KEEP_BIN=0
COVERAGE=0

for arg in "$@"; do
    case "$arg" in
        --verbose)  VERBOSE=1  ;;
        --keep-bin) KEEP_BIN=1 ;;
        --coverage) COVERAGE=1 ; KEEP_BIN=1 ;;  # [P2-4] keep objects for gcov
        --help)
            echo "Usage: $0 [--verbose] [--keep-bin] [--coverage]"
            echo "  --verbose   Print full output of each test binary."
            echo "  --keep-bin  Do not delete compiled test binaries after run."
            echo "  --coverage  Compile with gcov + generate HTML report."
            exit 0
            ;;
    esac
done

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
CFLAGS=(
    "-std=c11"
    "-g"
    "-O0"
    "-Wall"
    "-Wextra"
    "-Wno-unused-parameter"
    # Activate test-only hooks
    "-DUNIT_TEST=1"
    "-DNVM_STORE_HOST_MOCK=1"
    # Enable CAN FD ISO-TP paths so the test_isotp_canfd suite runs.
    # Production integrators set this flag themselves when FD is needed.
    "-DISOTP_ENABLE_CAN_FD=1"
    # Enable TX padding so the test_isotp_padding suite runs.
    # Default is off; this flag proves the padding code path compiles and passes.
    "-DISOTP_TX_PADDING=1"
    # Zephyr shim: replaces <zephyr/kernel.h> includes with no-ops
    "-DZTEST_HOST_SHIM=1"
    # Suppress the uds_msg_buf_t stack-size _Static_assert on the host build.
    # Host builds use the default 8 MB process stack — the guard is irrelevant
    # here. The assert fires on embedded targets where the limit defaults to
    # 256 bytes (smaller than uds_msg_buf_t), which is the intended behaviour.
    # See core/uds_types.h for the full rationale.
    "-DEDS_MSG_BUF_MAX_STACK_BYTES=8192"
)

# [P2-4] Append gcov instrumentation flags when --coverage is active.
if [[ $COVERAGE -eq 1 ]]; then
    CFLAGS+=(
        "-fprofile-arcs"
        "-ftest-coverage"
        "--coverage"
    )
    COVERAGE_DIR="${BUILD_DIR}/coverage"
    mkdir -p "${COVERAGE_DIR}"
fi

# Inject ztest_shim.h before every translation unit so that all
# ztest_* macros resolve without the Zephyr headers present.
SHIM_INCLUDE="-include ${ROOT}/tests/runner/ztest_shim.h"

# ---------------------------------------------------------------------------
# [SEC-TRNG-FAILCLOSED-01] Per-test extra compiler flags.
#
# Almost every test module compiles with the shared CFLAGS above. A small
# number of tests need to force a specific build-mode gate that cannot be
# reached from the default dev-configuration build — for example,
# test_trng_fail_closed proves the PRODUCTION (fail-closed) entropy
# behaviour of core/uds_security_algo.c, which only activates when
# CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY is defined as 0 (see the
# ALGO_ENTROPY_FAIL_CLOSED gate in that file). Keyed on the test name ($t
# in the build loop below).
# ---------------------------------------------------------------------------
extra_flags_for_test() {
    case "$1" in
        test_trng_fail_closed)
            # Forces ALGO_ENTROPY_FAIL_CLOSED=1 in this TU so the production
            # fail-closed path is exercised on a host build.
            echo "-DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0"
            ;;
        *)
            echo ""
            ;;
    esac
}

# ---------------------------------------------------------------------------
# Include paths (mirrors CMakeLists.txt target_include_directories)
# ---------------------------------------------------------------------------
INCLUDES=(
    "-I${ROOT}/core"
    "-I${ROOT}/core/uds_services"
    "-I${ROOT}/transport"
    "-I${ROOT}/config"
    "-I${ROOT}/platform"
    "-I${ROOT}/platform/zephyr"
    "-I${ROOT}/examples/basic_ecu/generated"
    "-I${ROOT}/project/unity"
    "-I${ROOT}/tests/runner"
    "-I${ROOT}/tests/mocks"
    "-I${ROOT}/transport/doip"
)

# ---------------------------------------------------------------------------
# Production stack source files
# Every test module links against the full stack so that integration between
# modules is exercised, not just the module under test in isolation.
# ---------------------------------------------------------------------------
STACK_SRCS=(
    # UDS core
    "${ROOT}/core/uds_server.c"
    "${ROOT}/core/uds_session.c"
    "${ROOT}/core/uds_session_stats.c"
    "${ROOT}/core/uds_security.c"
    "${ROOT}/core/uds_aes_cmac.c"          # [P1-SEC] AES-128-CMAC primitive
    "${ROOT}/core/uds_security_algo.c"
    "${ROOT}/core/uds_security_nvm.c"
    "${ROOT}/core/uds_safety.c"
    "${ROOT}/core/uds_access_table.c"
    "${ROOT}/core/uds_comm_control.c"
    # Service handlers
    "${ROOT}/core/uds_services/service_registration.c"
    "${ROOT}/core/uds_services/service_0x10.c"
    "${ROOT}/core/uds_services/service_0x11.c"
    "${ROOT}/core/uds_services/service_0x14.c"
    "${ROOT}/core/uds_services/service_0x19.c"
    "${ROOT}/core/uds_services/service_0x22.c"
    "${ROOT}/core/uds_services/service_0x27.c"
    "${ROOT}/core/uds_services/service_0x28.c"
    "${ROOT}/core/uds_services/service_0x2A.c"   # [0x2A] ReadDataByPeriodicIdentifier
    "${ROOT}/core/uds_periodic.c"                 # [0x2A] periodic scheduler
    "${ROOT}/core/uds_services/service_0x2E.c"
    "${ROOT}/core/uds_services/service_0x2F.c"   # InputOutputControlByIdentifier
    "${ROOT}/core/uds_services/service_0x31.c"
    "${ROOT}/core/uds_services/service_0x3E.c"
    "${ROOT}/core/uds_services/service_0x85.c"
    # DFU services — added when 0x34/0x36/0x37 were implemented.
    # service_registration.c references all three handlers in g_uds_service_table[];
    # omitting these causes every test binary to fail at link with:
    #   undefined reference to `uds_service_0x34_handler'
    # [FIX-DFU-SRCS] — root cause of "all tests BUILD_FAIL" regression.
    "${ROOT}/core/uds_services/service_0x23.c"   # ReadMemoryByAddress
    "${ROOT}/core/uds_services/service_0x34.c"   # RequestDownload
    "${ROOT}/core/uds_services/service_0x35.c"   # RequestUpload
    "${ROOT}/core/uds_services/service_0x36.c"   # TransferData
    "${ROOT}/core/uds_services/service_0x37.c"   # RequestTransferExit
    "${ROOT}/core/uds_services/service_0x3D.c"   # WriteMemoryByAddress
    "${ROOT}/core/uds_transfer_ctx.c"             # DFU transfer state machine
    "${ROOT}/platform/uds_flash_ops.c"            # Flash ops singleton
    # Transport
    "${ROOT}/transport/isotp.c"
    "${ROOT}/transport/can_transport.c"
    # DoIP transport (Week 1 — core logic only, no platform/network deps)
    "${ROOT}/transport/doip/doip_server.c"
    # Config databases
    "${ROOT}/config/did_database.c"
    "${ROOT}/config/dtc_database.c"
    "${ROOT}/config/dtc_mirror.c"
    "${ROOT}/config/routine_database.c"
    # Generated sources (must have run codegen first)
    "${ROOT}/examples/basic_ecu/generated/did_handlers.c"
    "${ROOT}/examples/basic_ecu/generated/did_safety_wrappers.c"
    "${ROOT}/examples/basic_ecu/generated/routine_handlers.c"
    # Platform: host mock replaces Zephyr flash driver
    "${ROOT}/platform/zephyr/nvm_store_mock.c"
    # Host mocks for Zephyr kernel APIs
    "${ROOT}/tests/mocks/zephyr_port_mock.c"
    # Unity test framework
    "${ROOT}/project/unity/unity.c"
    # Shared test runner (main + setUp/tearDown wiring)
    "${ROOT}/tests/runner/test_main.c"
)

# ---------------------------------------------------------------------------
# Test modules — one entry per file in tests/unit_runnable/
# ---------------------------------------------------------------------------
TESTS=(
    test_uds_session
    test_uds_security
    test_uds_safety
    test_uds_server
    test_did_database
    test_dtc_database
    test_dtc_mirror
    test_routine_database
    test_did_handlers
    test_did_safety_wrappers
    test_can_transport
    test_isotp
    test_isotp_concurrent
    test_service_0x10
    test_service_0x11
    test_service_0x14
    test_service_0x19
    test_service_0x22
    test_service_0x27
    test_service_0x28
    test_uds_periodic
    test_service_0x2A
    test_service_0x2F
    test_service_0x31
    test_service_0x23_0x3D
    test_service_0x34
    test_service_0x35
    test_service_0x36
    test_service_0x37
    test_service_0x3E
    test_service_0x85
    test_phase2_suppress_bit
    test_phase2_session_matrix
    test_phase2_isotp_stmin
    test_phase3_nvm_security
    test_phase3_nvm_dtc
    test_phase3_nvm_integration
    test_phase5_security_algo
    test_phase5_access_table
    test_phase5_server_access
    test_phase5_replay_protection
    test_doip_server
    # [SEC-TRNG-FAILCLOSED-01] Production-configuration entropy fail-closed
    # behaviour. Compiled separately (see extra_flags_for_test above) with
    # CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 because that behaviour cannot be
    # reached from the default dev-configuration build used by every other
    # test in this list.
    test_trng_fail_closed
)

# ---------------------------------------------------------------------------
# Sanity check: ensure generated files exist
# ---------------------------------------------------------------------------
if [[ ! -f "${ROOT}/examples/basic_ecu/generated/did_handlers.c" ]]; then
    echo ""
    echo "ERROR: examples/basic_ecu/generated/did_handlers.c not found."
    echo "       Run code generation first:"
    echo "         python3 tools/codegen.py \\"
    echo "             --config examples/basic_ecu/diagnostics_config.yaml \\"
    echo "             --out examples/basic_ecu/generated/ --safety-wrappers --asil-level B --no-manifest"
    echo ""
    exit 1
fi

if [[ ! -f "${ROOT}/examples/basic_ecu/generated/safety_config.h" ]]; then
    echo ""
    echo "ERROR: examples/basic_ecu/generated/safety_config.h not found."
    echo "       Run codegen with --safety-wrappers:"
    echo "         python3 tools/codegen.py \\"
    echo "             --config examples/basic_ecu/diagnostics_config.yaml \\"
    echo "             --out examples/basic_ecu/generated/ --safety-wrappers --asil-level B --no-manifest"
    echo ""
    exit 1
fi

# ---------------------------------------------------------------------------
# Build and run
# ---------------------------------------------------------------------------
mkdir -p "${BUILD_DIR}"

PASS=0
FAIL=0
TOTAL=${#TESTS[@]}

echo ""
echo "================================================================"
echo "  Xaloqi EDS — Host Unit Tests  (${TOTAL} modules)"
echo "================================================================"
echo ""

for t in "${TESTS[@]}"; do
    test_src="${ROOT}/tests/unit_runnable/${t}.c"
    bin="${BUILD_DIR}/${t}"

    # [SEC-TRNG-FAILCLOSED-01] Per-test extra flags (empty for most tests).
    read -r -a extra_flags <<< "$(extra_flags_for_test "${t}")"

    # ── Build ──────────────────────────────────────────────────────────
    # [FIX-SETE] With set -euo pipefail active, `var=$(failing_cmd)` exits
    # the outer script immediately — build_rc=$? is never reached.
    # The `&& rc=0 || rc=$?` idiom captures exit code correctly without
    # triggering set -e. Root cause of all-tests silent BUILD_FAIL.
    build_out=$(
        gcc "${CFLAGS[@]}" "${extra_flags[@]}" ${SHIM_INCLUDE} "${INCLUDES[@]}" \
            "${test_src}" \
            "${STACK_SRCS[@]}" \
            -o "${bin}" 2>&1
    ) && build_rc=0 || build_rc=$?

    if [[ $build_rc -ne 0 ]]; then
        printf "  %-45s  BUILD_FAIL\n" "${t}"
        if [[ $VERBOSE -eq 1 ]]; then
            echo "    --- build output ---"
            echo "$build_out" | sed 's/^/    /'
        fi
        FAIL=$((FAIL + 1))
        continue
    fi

    # ── Run ────────────────────────────────────────────────────────────
    # [FIX-SETE] Same set -e + $() trap applies to test binary execution.
    run_out=$(timeout 30 "${bin}" 2>&1) && run_rc=0 || run_rc=$?

    if [[ $run_rc -eq 0 ]]; then
        printf "  %-45s  PASS\n" "${t}"
        PASS=$((PASS + 1))
    else
        printf "  %-45s  FAIL\n" "${t}"
        if [[ $VERBOSE -eq 1 ]]; then
            echo "    --- test output ---"
            echo "$run_out" | grep -E "FAIL|ERROR|assert" | sed 's/^/    /' || true
        else
            echo "$run_out" | grep -E "FAIL|ERROR" | head -3 | sed 's/^/    /' || true
        fi
        FAIL=$((FAIL + 1))
    fi
done

# ---------------------------------------------------------------------------
# [ISSUE-87] Anti-drift guard: every ZTEST(suite, name) case in
# tests/unit_runnable/*.c must have a matching RUN_TEST(...) call inside
# that file's run_all_tests(). A ZTEST case with no RUN_TEST compiles fine
# but silently never executes -- the module still reports PASS even though
# the case never ran. This guard fails the build if that drift recurs.
#
# [FIX-SETE] grep/comm returning "no matches" exit non-zero; under
# set -euo pipefail that would abort the script inside a $(...) pipeline,
# so every such command here is explicitly allowed to "fail" with `|| true`.
# LC_ALL=C keeps sort/comm ordering stable regardless of the host locale.
# ---------------------------------------------------------------------------
WIRING_FAIL=0
for f in "${ROOT}"/tests/unit_runnable/*.c; do
    defined=$(LC_ALL=C grep -oP '^ZTEST\(\s*\K[A-Za-z0-9_]+\s*,\s*[A-Za-z0-9_]+' "${f}" 2>/dev/null \
        | sed 's/\s*,\s*/__/' | LC_ALL=C sort -u) || true
    wired=$(sed -n '/void run_all_tests/,/^}/p' "${f}" \
        | LC_ALL=C grep -oP 'RUN_TEST\(\s*\K[A-Za-z0-9_]+' 2>/dev/null | LC_ALL=C sort -u) || true
    missing=$(LC_ALL=C comm -23 <(echo "${defined}") <(echo "${wired}")) || true

    if [[ -n "${missing}" ]]; then
        echo "ERROR: ${f} has ZTEST case(s) with no matching RUN_TEST in run_all_tests():"
        echo "${missing}" | sed 's/^/    /'
        WIRING_FAIL=1
    fi
done

if [[ $WIRING_FAIL -ne 0 ]]; then
    echo ""
    echo "FAIL: One or more ZTEST cases are defined but never wired into run_all_tests()."
    echo "      They compile but never execute (see issue #87)."
    exit 1
else
    echo ""
    echo "PASS: All ZTEST cases in tests/unit_runnable/*.c are wired into run_all_tests()."
fi

# ---------------------------------------------------------------------------
# [SEC-BUILD-MODE-01 / issue #84] EDS_BUILD_IS_PRODUCTION macro probe.
#
# Compiles tests/probe_eds_build_is_production.c under every build-mode -D
# combination the codebase can present and checks the macro resolves
# correctly in each -- see scripts/verify_build_mode_macro.sh for the full
# truth table and rationale.
# ---------------------------------------------------------------------------
echo ""
if bash "${ROOT}/scripts/verify_build_mode_macro.sh"; then
    BUILD_MODE_PROBE_FAIL=0
else
    BUILD_MODE_PROBE_FAIL=1
fi

# ---------------------------------------------------------------------------
# [SEC-KEY-GATE-01 / CRIT-4 / issue #84] Negative compile test.
#
# Proves the CRIT-4 placeholder-key #error is now actually REACHABLE in a
# real production build -- it never was before this fix (the old
# `defined(X) && !X` idiom never fires when Zephyr's autoconf.h omits the
# symbol for a Kconfig bool set to `n`, which is exactly what a real
# production build looks like).
#
# CASE A (negative): compiling core/uds_security_algo.c with
# -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 and WITHOUT -DUNIT_TEST (i.e. not a
# host-test build -- see the CRIT-4 gate's own `!defined(UNIT_TEST)`
# rationale in uds_security_algo.c) must FAIL to compile, with an error
# mentioning SEC-KEY-GATE-01.
#
# No separate regression-guard compile: the default dev-mode compile
# (-DUNIT_TEST=1, no CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY) is already proven by
# every one of the ~42 test modules built in the main loop above, all of
# which link core/uds_security_algo.c under that exact configuration -- a
# second compile here would only re-prove a fact the main loop already
# established, on every single invocation of this script.
# ---------------------------------------------------------------------------
CRIT4_TMP_OBJ="$(mktemp -u /tmp/eds_crit4_negtest.XXXXXX.o)"
# Reuses INCLUDES (defined above) rather than its own copy -- a second,
# independently-maintained include-path array is exactly the "two copies of
# the same fact drift apart" failure mode this PR fixes elsewhere; no reason
# to reintroduce it here.

echo ""
echo "================================================================"
echo "  [SEC-KEY-GATE-01] CRIT-4 negative compile test"
echo "================================================================"
echo ""

crit4_neg_out=$(
    gcc -std=c11 -c -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 -DEDS_MSG_BUF_MAX_STACK_BYTES=8192         "${INCLUDES[@]}" "${ROOT}/core/uds_security_algo.c" -o "${CRIT4_TMP_OBJ}" 2>&1
) && crit4_neg_rc=0 || crit4_neg_rc=$?
rm -f "${CRIT4_TMP_OBJ}"

CRIT4_FAIL=0
if [[ ${crit4_neg_rc} -ne 0 ]] && echo "${crit4_neg_out}" | grep -q "SEC-KEY-GATE-01"; then
    echo "  PASS: production build (CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0, no UNIT_TEST)"
    echo "        correctly FAILS to compile, citing SEC-KEY-GATE-01."
else
    echo "  FAIL: expected a SEC-KEY-GATE-01 compile failure, got:"
    echo "        rc=${crit4_neg_rc}"
    echo "${crit4_neg_out}" | sed 's/^/        /'
    CRIT4_FAIL=1
fi
echo ""

if [[ ${BUILD_MODE_PROBE_FAIL} -ne 0 || ${CRIT4_FAIL} -ne 0 ]]; then
    echo "FAIL: build-mode gate verification (SEC-BUILD-MODE-01 / SEC-KEY-GATE-01) failed."
    exit 1
fi

# ---------------------------------------------------------------------------
# [P2-4] Coverage report
# ---------------------------------------------------------------------------
if [[ $COVERAGE -eq 1 ]]; then
    echo ""
    echo "================================================================"
    echo "  Coverage Report (lcov + genhtml)"
    echo "================================================================"

    # Collect coverage data from all .gcda files produced by running the tests.
    lcov \
        --capture \
        --directory "${BUILD_DIR}" \
        --output-file "${COVERAGE_DIR}/raw.info" \
        --rc lcov_branch_coverage=1 \
        2>/dev/null || { echo "  WARN: lcov not installed — skipping HTML report."; COVERAGE=0; }

    if [[ $COVERAGE -eq 1 ]]; then
        # Strip third-party and system headers; keep only EDS source.
        lcov \
            --remove "${COVERAGE_DIR}/raw.info" \
                "/usr/*" \
                "*/tests/*" \
                "*/project/unity/*" \
                "*/tests/runner/*" \
                "*/tests/mocks/*" \
            --output-file "${COVERAGE_DIR}/filtered.info" \
            --rc lcov_branch_coverage=1 \
            2>/dev/null

        # Generate HTML report.
        genhtml \
            "${COVERAGE_DIR}/filtered.info" \
            --output-directory "${COVERAGE_DIR}/html" \
            --title "EDS Unit Test Coverage" \
            --legend \
            --branch-coverage \
            --rc lcov_branch_coverage=1 \
            2>/dev/null

        echo "  HTML report: ${COVERAGE_DIR}/html/index.html"

        # [P2-4] Safety-critical module threshold check.
        # Extract line coverage % for key modules and fail if below threshold.
        python3 - "${COVERAGE_DIR}/filtered.info" << 'PYCHECK'
import sys, re
info_file = sys.argv[1]
thresholds = {
    "uds_safety.c":          95.0,
    "did_safety_wrappers.c": 95.0,
    "dtc_mirror.c":          90.0,
}
default_threshold = 80.0
coverage = {}   # filename -> (hit, total)
current = None
with open(info_file) as f:
    for line in f:
        if line.startswith("SF:"):
            current = line[3:].strip().split("/")[-1]
        elif line.startswith("LH:") and current:
            coverage.setdefault(current, [0, 0])[0] = int(line[3:])
        elif line.startswith("LF:") and current:
            coverage.setdefault(current, [0, 0])[1] = int(line[3:])
fail = False
for fname, (hit, total) in sorted(coverage.items()):
    if total == 0:
        continue
    pct = 100.0 * hit / total
    threshold = thresholds.get(fname, default_threshold)
    status = "OK " if pct >= threshold else "LOW"
    print(f"  {status}  {fname:<40s}  {pct:5.1f}%  (threshold {threshold:.0f}%)")
    if pct < threshold:
        fail = True
if fail:
    print("")
    print("FAIL: One or more modules below coverage threshold.")
    sys.exit(1)
else:
    print("")
    print("PASS: All modules meet coverage thresholds.")
PYCHECK
    fi
fi

# ---------------------------------------------------------------------------
# Cleanup (optional)
# ---------------------------------------------------------------------------
if [[ $KEEP_BIN -eq 0 ]]; then
    rm -rf "${BUILD_DIR}"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "================================================================"
echo "  === Unit Test Summary: ${PASS} passed, ${FAIL} failed ==="
echo "================================================================"
echo ""

[[ $FAIL -eq 0 ]]
