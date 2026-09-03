#!/usr/bin/env bash
# =============================================================================
# Xaloqi EDS
# build_tests.sh  (project root)
#
# PURPOSE: Build and run every host-side C unit test module in
#          tests/unit_runnable/. The full diagnostics stack is linked into
#          every test binary, so each module exercises integration between
#          modules and not just the unit under test.
#
# [#151] BUILD ONCE, LINK N TIMES
#   This script used to compile all ~44 stack sources from scratch for every
#   single test module (~44 x ~44 = ~1,900 translation units per run). The
#   stack is now compiled once into a static archive, and each test module
#   contributes only its own small driver .c file:
#
#       STACK_SRCS  --(compiled once per configuration)-->  libeds_testable.a
#       test_foo.c  --(compiled per module)--------------->  test_foo
#
#   Two modules need the shared sources built in a DIFFERENT compile-time
#   configuration and therefore get their own archive variant — see
#   extra_flags_for_test() below for the full rationale. The variant id is
#   derived from the flags themselves, so adding a third per-module -D
#   automatically produces a third archive instead of silently reusing the
#   default one and testing the wrong configuration.
#
#   The archive is pulled in with --whole-archive so that EVERY object is
#   linked into every test binary, exactly as when the sources were listed
#   on the command line. Without it, the linker would only extract archive
#   members that resolve an already-undefined symbol, which would silently
#   change link semantics (weak/strong interposition, e.g. test_main.c's
#   weak setUp/tearDown, and objects reachable only through data tables).
#   On a linker without --whole-archive the object files are listed
#   directly instead, which has the same all-objects-linked semantics.
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
#   bash build_tests.sh              # Build + run every test module
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
SANITIZE=0

for arg in "$@"; do
    case "$arg" in
        --verbose)  VERBOSE=1  ;;
        --keep-bin) KEEP_BIN=1 ;;
        --coverage) COVERAGE=1 ; KEEP_BIN=1 ;;  # [P2-4] keep objects for gcov
        --sanitize) SANITIZE=1 ;;               # [#151] ASan + UBSan
        --help)
            echo "Usage: $0 [--verbose] [--keep-bin] [--coverage] [--sanitize]"
            echo "  --verbose   Print full output of each test binary."
            echo "  --keep-bin  Do not delete compiled test binaries after run."
            echo "  --coverage  Compile with gcov + generate HTML report."
            echo "  --sanitize  Compile and run under AddressSanitizer +"
            echo "              UndefinedBehaviorSanitizer (see the CI"
            echo "              'sanitizers' job). Not combinable with --coverage."
            exit 0
            ;;
    esac
done

# gcov instrumentation and the sanitizer runtimes both rewrite the same code
# paths; mixing them produces coverage data nobody reads and sanitizer reports
# nobody can attribute. Refuse the combination rather than silently favouring
# one of them.
if [[ $COVERAGE -eq 1 && $SANITIZE -eq 1 ]]; then
    echo "ERROR: --coverage and --sanitize cannot be combined."
    exit 1
fi

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

# ---------------------------------------------------------------------------
# [#151] Sanitizer instrumentation.
#
# core/, transport/, config/ and platform/ are hand-managed C with a strict
# no-malloc/no-recursion policy, which makes memory-safety and UB errors both
# more likely to be silent and more expensive to find later. The shared stack
# archive from #151 made a second full instrumented build cheap enough to run
# on every PR, so CI now does (job 'sanitizers' in .github/workflows/ci.yml).
#
# -fno-sanitize-recover=all is what makes this a GATE rather than a log:
# UBSan's default behaviour is to print a one-line "runtime error:" and CARRY
# ON with exit status 0, so a job that only checked the exit code would go
# green on a real undefined-behaviour finding. With this flag the process
# aborts on the first violation. ASAN_OPTIONS/UBSAN_OPTIONS in the CI job
# harden the same property from the runtime side.
# ---------------------------------------------------------------------------
if [[ $SANITIZE -eq 1 ]]; then
    CFLAGS+=(
        "-fsanitize=address,undefined"
        "-fno-sanitize-recover=all"
        "-fno-omit-frame-pointer"
    )
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
#
# [#151] These flags change the compiled code of SHARED stack sources, not
# just the test driver:
#   CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY -> core/uds_security_algo.{c,h},
#                                        examples/basic_ecu/generated/uds_init.c
#   UDS_ACL_ALLOW_UNLISTED_SERVICES   -> core/uds_access_table.{c,h},
#                                        core/uds_server.c
# A module listed here therefore CANNOT link against the default
# libeds_testable.a — it would silently test the default configuration under
# a test name that claims to prove the other one. Each distinct flag set gets
# its own archive variant instead (see build_stack_lib below). This function
# is the single source of truth for both the driver's flags and its archive's
# flags, so the two can never skew apart.
# ---------------------------------------------------------------------------
extra_flags_for_test() {
    case "$1" in
        test_trng_fail_closed)
            # Forces ALGO_ENTROPY_FAIL_CLOSED=1 in this TU so the production
            # fail-closed path is exercised on a host build.
            echo "-DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0"
            ;;
        test_uds_acl_permissive_opt_in)
            # [#113] Forces the pre-#113 permissive-by-default ACL behaviour
            # so the opt-in itself is exercised on a host build; every other
            # test module compiles against the fail-closed default.
            echo "-DUDS_ACL_ALLOW_UNLISTED_SERVICES=1"
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
    test_service_0x2E
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
    # [#113] Permissive opt-in (UDS_ACL_ALLOW_UNLISTED_SERVICES=1) proof.
    # Compiled separately (see extra_flags_for_test above) because that
    # behaviour cannot be reached from the default (fail-closed) build used
    # by every other test in this list, including test_phase5_access_table.
    test_uds_acl_permissive_opt_in
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

# ===========================================================================
# [#151] Shared stack archive(s) — compiled once, linked into every module.
# ===========================================================================

LIB_DIR="${BUILD_DIR}/lib"

# --- Linker capability probe ------------------------------------------------
# --whole-archive is a GNU ld / lld flag. Probe for it once rather than
# assuming it: on a linker that lacks it (Apple ld, for instance) the object
# files are listed on the link line directly, which has identical
# all-objects-are-linked semantics, just without the archive indirection.
# Set EDS_TEST_FORCE_NO_WHOLE_ARCHIVE=1 to exercise that fallback on a GNU
# host — it is how the fallback path gets tested rather than assumed.
WHOLE_ARCHIVE_OK=0
if [[ "${EDS_TEST_FORCE_NO_WHOLE_ARCHIVE:-0}" != "1" ]]; then
    if printf 'int main(void){return 0;}\n' \
        | gcc -x c - -Wl,--whole-archive -Wl,--no-whole-archive -o /dev/null 2>/dev/null
    then
        WHOLE_ARCHIVE_OK=1
    fi
fi

# Archive variant id for a test module, derived from its own extra flags so
# that a new per-module -D cannot silently reuse the default archive.
variant_id_for_test() {
    local flags
    flags="$(extra_flags_for_test "$1")"
    if [[ -z "${flags//[[:space:]]/}" ]]; then
        echo "default"
    else
        printf '%s' "${flags}" \
            | tr -cs 'A-Za-z0-9' '_' \
            | sed -e 's/^_*//' -e 's/_*$//' \
            | tr 'A-Z' 'a-z'
    fi
}

archive_path_for_variant() {
    if [[ "$1" == "default" ]]; then
        echo "${LIB_DIR}/libeds_testable.a"
    else
        echo "${LIB_DIR}/libeds_testable__$1.a"
    fi
}

# variant id -> newline-separated link arguments that pull in the whole stack
declare -A STACK_LINK_ARGS=()
# variant id -> the extra compile flags that define it (empty for 'default')
declare -A VARIANT_FLAGS=()
# variant id -> space-separated list of the test modules that use it
declare -A VARIANT_TESTS=()

build_stack_lib() {
    local variant="$1"
    shift
    local -a vflags=("$@")

    local objdir="${LIB_DIR}/${variant}"
    local archive
    archive="$(archive_path_for_variant "${variant}")"

    mkdir -p "${objdir}"
    rm -f "${archive}"

    local -a objs=()
    local src rel obj out rc
    for src in "${STACK_SRCS[@]}"; do
        rel="${src#"${ROOT}/"}"
        # Full relative path in the object name: two sources with the same
        # basename in different directories must not collide.
        obj="${objdir}/${rel//\//_}.o"
        out=$(
            gcc "${CFLAGS[@]}" "${vflags[@]}" ${SHIM_INCLUDE} "${INCLUDES[@]}" \
                -c "${src}" -o "${obj}" 2>&1
        ) && rc=0 || rc=$?
        if [[ ${rc} -ne 0 ]]; then
            echo "ERROR: failed to compile ${rel} into stack variant '${variant}'."
            echo "${out}" | sed 's/^/    /'
            return 1
        fi
        objs+=("${obj}")
    done

    out=$(ar rcs "${archive}" "${objs[@]}" 2>&1) && rc=0 || rc=$?
    if [[ ${rc} -ne 0 ]]; then
        echo "ERROR: ar failed for stack variant '${variant}'."
        echo "${out}" | sed 's/^/    /'
        return 1
    fi

    # [run-013] Positive assertion, not merely "nothing errored": the archive
    # must exist AND contain exactly one member per stack source. An archive
    # that silently came out short would otherwise link fine for most modules
    # and only surface as a mystery undefined-reference much later.
    local members
    members=$(ar t "${archive}" | wc -l)
    if [[ "${members}" -ne "${#STACK_SRCS[@]}" ]]; then
        echo "ERROR: ${archive##*/} has ${members} member(s), expected ${#STACK_SRCS[@]}."
        return 1
    fi

    if [[ ${WHOLE_ARCHIVE_OK} -eq 1 ]]; then
        STACK_LINK_ARGS["${variant}"]=$(
            printf '%s\n' "-Wl,--whole-archive" "${archive}" "-Wl,--no-whole-archive"
        )
    else
        STACK_LINK_ARGS["${variant}"]=$(printf '%s\n' "${objs[@]}")
    fi
    return 0
}

PASS=0
FAIL=0
TOTAL=${#TESTS[@]}
# [run-013] Aggregate assertion counters — see the positive success check
# after the run loop.
CASES_RUN=0
CASES_FAILED=0
ZERO_CASE_MODULES=()

# Group the modules by the archive variant each one needs.
for t in "${TESTS[@]}"; do
    v="$(variant_id_for_test "${t}")"
    VARIANT_FLAGS["${v}"]="$(extra_flags_for_test "${t}")"
    VARIANT_TESTS["${v}"]="${VARIANT_TESTS[${v}]:-}${t} "
done

echo ""
echo "================================================================"
echo "  Shared stack archives  (${#STACK_SRCS[@]} sources x ${#VARIANT_FLAGS[@]} configuration(s))"
echo "================================================================"
echo ""
if [[ ${WHOLE_ARCHIVE_OK} -eq 1 ]]; then
    echo "  Link mode: --whole-archive (every archive member is linked)"
else
    echo "  Link mode: explicit object list (--whole-archive unavailable)"
fi
echo ""

LIB_FAIL=0
for v in "${!VARIANT_FLAGS[@]}"; do
    read -r -a vflags <<< "${VARIANT_FLAGS[${v}]}"
    if build_stack_lib "${v}" "${vflags[@]}"; then
        n_modules=$(wc -w <<< "${VARIANT_TESTS[${v}]}")
        if [[ "${v}" == "default" ]]; then
            printf "  %-38s  OK  (%s module(s), default configuration)\n" \
                "$(basename "$(archive_path_for_variant "${v}")")" "${n_modules}"
        else
            printf "  %-38s  OK  (%s module(s): %s)\n" \
                "$(basename "$(archive_path_for_variant "${v}")")" \
                "${n_modules}" "${VARIANT_FLAGS[${v}]}"
        fi
    else
        LIB_FAIL=1
    fi
done
echo ""

if [[ ${LIB_FAIL} -ne 0 ]]; then
    echo "FAIL: one or more shared stack archives failed to build."
    exit 1
fi

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
    # [#151] ...and the stack archive built with those very same flags.
    variant="$(variant_id_for_test "${t}")"
    mapfile -t stack_link_args <<< "${STACK_LINK_ARGS[${variant}]}"

    # ── Build ──────────────────────────────────────────────────────────
    # [FIX-SETE] With set -euo pipefail active, `var=$(failing_cmd)` exits
    # the outer script immediately — build_rc=$? is never reached.
    # The `&& rc=0 || rc=$?` idiom captures exit code correctly without
    # triggering set -e. Root cause of all-tests silent BUILD_FAIL.
    build_out=$(
        gcc "${CFLAGS[@]}" "${extra_flags[@]}" ${SHIM_INCLUDE} "${INCLUDES[@]}" \
            "${test_src}" \
            "${stack_link_args[@]}" \
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

    # [run-013] Count the assertions each module actually executed, not just
    # whether the process exited 0. A module whose run_all_tests() executed
    # nothing at all still exits 0 and would otherwise be indistinguishable
    # from one that ran its whole suite. The per-module floor is enforced
    # after the loop.
    module_cases=$(sed -n 's/^Tests run:[[:space:]]*\([0-9][0-9]*\).*/\1/p' <<< "${run_out}" | tail -1)
    module_failed=$(sed -n 's/^Tests failed:[[:space:]]*\([0-9][0-9]*\).*/\1/p' <<< "${run_out}" | tail -1)
    if [[ -z "${module_cases}" ]]; then
        # No Unity summary at all — a crash, a sanitizer abort, or a binary
        # that never reached UNITY_END(). Recorded as zero so the positive
        # assertion below catches it.
        module_cases=0
        module_failed=0
        ZERO_CASE_MODULES+=("${t}")
    elif [[ "${module_cases}" -eq 0 ]]; then
        ZERO_CASE_MODULES+=("${t}")
    fi
    CASES_RUN=$((CASES_RUN + module_cases))
    CASES_FAILED=$((CASES_FAILED + module_failed))

    if [[ $run_rc -eq 0 ]]; then
        printf "  %-45s  PASS  (%s cases)\n" "${t}" "${module_cases}"
        PASS=$((PASS + 1))
    else
        printf "  %-45s  FAIL  (%s cases)\n" "${t}" "${module_cases}"
        if [[ $VERBOSE -eq 1 ]]; then
            echo "    --- test output ---"
            echo "$run_out" | grep -E "FAIL|ERROR|assert" | sed 's/^/    /' || true
        else
            echo "$run_out" | grep -E "FAIL|ERROR" | head -3 | sed 's/^/    /' || true
        fi
        # Sanitizer diagnostics are never truncated away: an ASan/UBSan report
        # is the entire reason the --sanitize build exists.
        if [[ $SANITIZE -eq 1 ]]; then
            echo "    --- sanitizer output ---"
            echo "$run_out" \
                | grep -E "Sanitizer|runtime error|SUMMARY:|#[0-9]+ 0x" \
                | head -40 | sed 's/^/    /' || true
        fi
        FAIL=$((FAIL + 1))
    fi
done

# ---------------------------------------------------------------------------
# [run-013] Positive success assertion.
#
# "0 failed" is not the same statement as "the suite ran". A module that
# executed zero assertions exits 0 and is counted as PASS by the loop above;
# so is a whole suite that somehow produced no test binaries at all. Assert
# what actually happened — every module reported a Unity summary, and every
# module ran at least one case — instead of only the absence of failures.
# ---------------------------------------------------------------------------
# Tracked separately from FAIL so that "N passed, M failed" keeps summing to
# the module count — a module that aborts is already counted once in FAIL.
ZERO_CASE_FAIL=0

if [[ ${#ZERO_CASE_MODULES[@]} -gt 0 ]]; then
    echo ""
    echo "FAIL: ${#ZERO_CASE_MODULES[@]} module(s) executed zero test cases:"
    printf '    %s\n' "${ZERO_CASE_MODULES[@]}"
    echo "      A module that runs nothing still exits 0 and would otherwise"
    echo "      be reported as PASS (see knowledge lessons/run-013)."
    ZERO_CASE_FAIL=1
fi

if [[ ${CASES_RUN} -lt ${TOTAL} ]]; then
    echo ""
    echo "FAIL: only ${CASES_RUN} test case(s) ran across ${TOTAL} module(s)."
    echo "      Every module must execute at least one assertion."
    ZERO_CASE_FAIL=1
fi

# ---------------------------------------------------------------------------
# [ISSUE-87] Anti-drift guard: every ZTEST(suite, name) case in
# tests/unit_runnable/*.c must have a matching RUN_TEST(...) call inside
# that file's run_all_tests(). A ZTEST case with no RUN_TEST compiles fine
# but silently never executes -- the module still reports PASS even though
# the case never ran. This guard fails the build if that drift recurs.
#
# [EDS#235] grep/comm returning "no matches" exit non-zero; under
# set -euo pipefail that would abort the script mid-pipeline, so each
# pipeline below is used as an `if !` condition (exempt from set -e) and
# its exit code inspected via PIPESTATUS -- "no match" (grep exit 1) is
# expected and fine, anything else is a real tool/I/O error and fails
# the gate closed instead of silently reporting PASS.
# Real temp files, not `<(...)` process substitution: this gate used to
# report PASS even when `/dev/fd` wasn't available to the shell, because
# `comm: /dev/fd/NN: No such file or directory` was swallowed by a
# blanket `|| true` on every step (see EDS#235 for the original report).
# LC_ALL=C keeps sort/comm ordering stable regardless of the host locale.
# ---------------------------------------------------------------------------
WIRING_FAIL=0
WIRING_TMP="$(mktemp -d)" || { echo "ERROR: mktemp failed for wiring gate"; exit 1; }
trap 'rm -rf "${WIRING_TMP}"' EXIT

for f in "${ROOT}"/tests/unit_runnable/*.c; do
    if ! LC_ALL=C grep -oP '^ZTEST\(\s*\K[A-Za-z0-9_]+\s*,\s*[A-Za-z0-9_]+' "${f}" \
            | sed 's/\s*,\s*/__/' | LC_ALL=C sort -u > "${WIRING_TMP}/defined"; then
        grep_rc=${PIPESTATUS[0]}
        if [[ ${grep_rc} -gt 1 ]]; then
            echo "ERROR: failed to scan ${f} for ZTEST definitions (grep exit ${grep_rc})"
            WIRING_FAIL=1
            continue
        fi
        : > "${WIRING_TMP}/defined"   # grep exit 1 = no ZTEST cases in this file; fine
    fi

    if ! sed -n '/void run_all_tests/,/^}/p' "${f}" \
            | LC_ALL=C grep -oP 'RUN_TEST\(\s*\K[A-Za-z0-9_]+' | LC_ALL=C sort -u > "${WIRING_TMP}/wired"; then
        grep_rc=${PIPESTATUS[1]}
        if [[ ${grep_rc} -gt 1 ]]; then
            echo "ERROR: failed to scan ${f} for RUN_TEST wiring (grep exit ${grep_rc})"
            WIRING_FAIL=1
            continue
        fi
        : > "${WIRING_TMP}/wired"     # grep exit 1 = no RUN_TEST calls in this file; fine
    fi

    if ! missing="$(LC_ALL=C comm -23 "${WIRING_TMP}/defined" "${WIRING_TMP}/wired")"; then
        echo "ERROR: comm failed comparing wiring for ${f} (exit $?)"
        WIRING_FAIL=1
        continue
    fi

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
# the default libeds_testable.a built above -- core/uds_security_algo.c is
# compiled into it under exactly that configuration, and every module that
# is not listed in extra_flags_for_test() links against it. A second compile
# here would only re-prove a fact the archive build already established, on
# every single invocation of this script.
# [#151] Before the shared-archive restructure this same fact was re-proven
# ~42 times per run (once per test module); it is now proven once. Nothing
# about the configuration under test changed.
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

# ---------------------------------------------------------------------------
# [EDS#215] Negative compile test — FreeRTOS RAM-stub flash production guard.
#
# Mirrors the CRIT-4 negative compile test above: proves the
# freertos_flash_ops.c RAM-stub #error (mirroring SEC-KEY-GATE-01) is
# actually reachable. Compiling the file with
# -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 (production), without -DUNIT_TEST,
# and without STM32H7xx/STM32H743xx defined (so the RAM stub backend is
# selected) must FAIL to compile, citing EDS#215.
#
# Only the STM32H7xx HAL backend is skipped here (that branch needs the
# vendored STM32Cube HAL headers this repo does not carry) — the RAM stub
# backend under test has no such dependency, so this compiles cleanly on
# the host like the CRIT-4 test above.
# ---------------------------------------------------------------------------
EDS215_TMP_OBJ="$(mktemp -u /tmp/eds_215_negtest.XXXXXX.o)"
EDS215_INCLUDES=(
    "${INCLUDES[@]}"
    "-I${ROOT}/platform/freertos"
)

echo "================================================================"
echo "  [EDS#215] FreeRTOS flash-stub production guard negative compile test"
echo "================================================================"
echo ""

eds215_neg_out=$(
    gcc -std=c11 -c -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 -DEDS_MSG_BUF_MAX_STACK_BYTES=8192 \
        "${EDS215_INCLUDES[@]}" "${ROOT}/platform/freertos/freertos_flash_ops.c" -o "${EDS215_TMP_OBJ}" 2>&1
) && eds215_neg_rc=0 || eds215_neg_rc=$?
rm -f "${EDS215_TMP_OBJ}"

EDS215_FAIL=0
if [[ ${eds215_neg_rc} -ne 0 ]] && echo "${eds215_neg_out}" | grep -q "EDS#215"; then
    echo "  PASS: production build (CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0, no UNIT_TEST,"
    echo "        no STM32H7xx) correctly FAILS to compile, citing EDS#215."
else
    echo "  FAIL: expected an EDS#215 compile failure, got:"
    echo "        rc=${eds215_neg_rc}"
    echo "${eds215_neg_out}" | sed 's/^/        /'
    EDS215_FAIL=1
fi
echo ""

# Regression guard: the same file, same production config, but with
# STM32H7xx defined (the real hardware backend) must NOT hit the RAM-stub
# gate. It is expected to fail for an unrelated reason (this repo does not
# vendor the STM32Cube HAL headers) — the check here is only that the
# failure is a missing-header error, never an EDS#215 gate firing on the
# real-hardware branch.
eds215_pos_out=$(
    gcc -std=c11 -c -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 -DEDS_MSG_BUF_MAX_STACK_BYTES=8192 -DSTM32H743xx \
        "${EDS215_INCLUDES[@]}" "${ROOT}/platform/freertos/freertos_flash_ops.c" -o "${EDS215_TMP_OBJ}" 2>&1
) && eds215_pos_rc=0 || eds215_pos_rc=$?
rm -f "${EDS215_TMP_OBJ}"

if echo "${eds215_pos_out}" | grep -q "EDS#215"; then
    echo "  FAIL: EDS#215 gate incorrectly fired on the STM32H7xx HAL backend"
    echo "        (production hardware build, not the RAM stub):"
    echo "${eds215_pos_out}" | sed 's/^/        /'
    EDS215_FAIL=1
elif [[ ${eds215_pos_rc} -ne 0 ]] && echo "${eds215_pos_out}" | grep -qi "stm32h7xx_hal.h"; then
    echo "  PASS: STM32H7xx HAL backend selected (not the RAM stub) — EDS#215"
    echo "        gate correctly did not fire; unrelated failure is the"
    echo "        expected missing vendored HAL header."
else
    echo "  FAIL: expected a missing-stm32h7xx_hal.h failure (or success), got:"
    echo "        rc=${eds215_pos_rc}"
    echo "${eds215_pos_out}" | sed 's/^/        /'
    EDS215_FAIL=1
fi
echo ""

if [[ ${BUILD_MODE_PROBE_FAIL} -ne 0 || ${CRIT4_FAIL} -ne 0 || ${EDS215_FAIL} -ne 0 ]]; then
    echo "FAIL: build-mode gate verification (SEC-BUILD-MODE-01 / SEC-KEY-GATE-01 / EDS#215) failed."
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
if [[ $SANITIZE -eq 1 ]]; then
    echo "  === Build: ASan + UBSan (-fsanitize=address,undefined) ==="
fi
# [run-013] The cases line states what actually ran; the summary line states
# what passed. A reader (or a CI gate) needs both — "0 failed" alone cannot
# distinguish a green suite from a suite that executed nothing.
echo "  === Unit Test Cases: ${CASES_RUN} run, ${CASES_FAILED} failed ==="
echo "  === Unit Test Summary: ${PASS} passed, ${FAIL} failed ==="
echo "================================================================"
echo ""

[[ $FAIL -eq 0 && $ZERO_CASE_FAIL -eq 0 ]]
