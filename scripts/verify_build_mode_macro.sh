#!/usr/bin/env bash
# =============================================================================
# Xaloqi EDS
# scripts/verify_build_mode_macro.sh
#
# PURPOSE: Permanent regression probe for EDS_BUILD_IS_PRODUCTION
#          (core/uds_security_algo.h, SEC-BUILD-MODE-01 / issue #84).
#
#          Compiles tests/probe_eds_build_is_production.c (a preprocessor-
#          only probe, never linked) under every build-mode -D combination
#          this codebase can present, and checks that EDS_BUILD_IS_PRODUCTION
#          resolves to the expected 0 (development) or 1 (production) value
#          in each case. A triggered #error in that file always fails the
#          gcc invocation regardless of warning/optimization flags, so this
#          is a reliable oracle for a compile-time-only macro that has no
#          runtime value to assert against directly.
#
#          This is the automated form of the truth table used to catch and
#          fix issue #84: the OLD `defined(X) && !X` idiom silently did
#          nothing on a real Zephyr production build (case 2) and left every
#          FreeRTOS build unprotected (case 4, before examples/*_freertos/
#          CMakeLists.txt started defining CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY
#          itself). Every case below must keep resolving correctly, forever.
#
# USAGE:
#   bash scripts/verify_build_mode_macro.sh
#
# EXIT CODES:
#   0  All cases resolved to their expected value.
#   1  One or more cases resolved incorrectly, or failed to compile at all.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROBE_SRC="${ROOT}/tests/probe_eds_build_is_production.c"
OBJ="$(mktemp -u /tmp/eds_build_mode_probe.XXXXXX.o)"

trap 'rm -f "${OBJ}"' EXIT

if [[ ! -f "${PROBE_SRC}" ]]; then
    echo "ERROR: ${PROBE_SRC} not found."
    exit 1
fi

# ---------------------------------------------------------------------------
# Each entry: "label|expected(0|1)|extra -D flags (space-separated, may be empty)"
# Mirrors the truth table in the #84 fix design / EDS_BUILD_IS_PRODUCTION's
# own doc comment.
# ---------------------------------------------------------------------------
CASES=(
    "Zephyr dev (CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=1)|0|-DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=1"
    "Zephyr production (symbol omitted)|1|"
    "FreeRTOS dev, new CMake default (EDS_PLATFORM_FREERTOS + CONFIG=1)|0|-DEDS_PLATFORM_FREERTOS=1 -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=1"
    "FreeRTOS with nothing wired (regression safety net)|1|-DEDS_PLATFORM_FREERTOS=1"
    "host unit-test / harness build (UNIT_TEST=1)|0|-DUNIT_TEST=1"
    "forced production test binary (UNIT_TEST=1, CONFIG=0)|1|-DUNIT_TEST=1 -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0"
    "explicit override escape hatch (EDS_BUILD_IS_PRODUCTION=1 wins)|1|-DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=1 -DEDS_BUILD_IS_PRODUCTION=1"
)

PASS=0
FAIL=0

echo ""
echo "================================================================"
echo "  Xaloqi EDS — EDS_BUILD_IS_PRODUCTION build-mode macro probe"
echo "================================================================"
echo ""

for entry in "${CASES[@]}"; do
    IFS='|' read -r label expected extra_flags <<< "${entry}"

    # shellcheck disable=SC2086
    build_out=$(gcc -I"${ROOT}/core" ${extra_flags} -c "${PROBE_SRC}" -o "${OBJ}" 2>&1) && build_rc=0 || build_rc=$?

    if [[ ${build_rc} -eq 0 ]]; then
        printf "  %-70s  BUILD_FAIL (no #error fired)\n" "${label}"
        FAIL=$((FAIL + 1))
        continue
    fi

    if echo "${build_out}" | grep -q "EDS_BUILD_IS_PRODUCTION_PROBE_RESULT_${expected}"; then
        printf "  %-70s  PASS (== %s)\n" "${label}" "${expected}"
        PASS=$((PASS + 1))
    else
        got="?"
        if echo "${build_out}" | grep -q "EDS_BUILD_IS_PRODUCTION_PROBE_RESULT_0"; then got="0"; fi
        if echo "${build_out}" | grep -q "EDS_BUILD_IS_PRODUCTION_PROBE_RESULT_1"; then got="1"; fi
        printf "  %-70s  FAIL (expected %s, got %s)\n" "${label}" "${expected}" "${got}"
        echo "${build_out}" | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
done

echo ""
echo "================================================================"
echo "  Build-mode macro probe: ${PASS} passed, ${FAIL} failed (of ${#CASES[@]})"
echo "================================================================"
echo ""

if [[ ${FAIL} -ne 0 ]]; then
    exit 1
fi
exit 0
