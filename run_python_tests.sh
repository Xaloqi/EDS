#!/usr/bin/env bash
# =============================================================================
# Xaloqi EDS
# run_python_tests.sh  (project root)
#
# PURPOSE: Canonical entrypoint for the whole Python test suite (issue #150).
#          Mirrors build_tests.sh's role for the C suite: one command, run
#          from anywhere in the repo, that exercises everything correctly
#          scoped and prints a clear pass/skip/fail summary.
#
# WHY THIS EXISTS INSTEAD OF A SHARED ROOT conftest.py:
#   Bare `pytest` from the repo root only collects tests/ (see pytest.ini —
#   testpaths = tests) because every examples/*/generated/tests/ directory
#   is a self-contained pytest project: its own committed pytest.ini (its
#   own rootdir) and its own conftest.py declaring
#   `pytest_plugins = ["conftest_firmware"]`, which pytest only allows in a
#   conftest.py sitting at that session's rootdir. Walking all 12 example
#   trees plus tests/ in one shared session breaks that. Those generated/
#   files are produced by tools/testgen.py (a commercial, gitignored
#   deliverable — CLAUDE.md/#67) and must never be hand-edited, so this
#   script runs each suite scoped to its own directory instead — exactly
#   the invocation README.md and CI already document
#   (`cd examples/<name>/generated/tests && pytest ...`).
#
# WHAT IT RUNS:
#   1. tests/                         — this repo's own hand-written suite
#   2. examples/*/generated/tests/    — each example's generated suite,
#      one pytest session per example directory (its own pytest.ini
#      applies: simulator mode by default, --strict-markers, etc.)
#
# SKIP CLASSIFICATION:
#   A suite that fails collection or has 1+ real test failures is FAIL.
#   A suite where every test passed (0 failed) is PASS, but if it produced
#   zero passes and only skips (an optional dependency — xaloqi-tester,
#   pycryptodome — or firmware_bus is unavailable, or a firmware binary/ECU
#   binary is missing) it is reported as [ENV] instead of a bare PASS, so
#   "nothing meaningful ran here because this environment is incomplete" is
#   never mistaken for "verified and all green". Look for "[ENV]" in the
#   underlying pytest skip reasons to grep the exact cause.
#
# USAGE:
#   bash run_python_tests.sh              # full suite (all examples + tests/)
#   bash run_python_tests.sh --quick       # skip basic_ecu's 12-file, ~110s
#                                          # robustness campaign (still runs
#                                          # in its own dedicated CI job)
#
# EXIT CODES:
#   0  No suite reported a real test failure or collection error.
#   1  At least one suite failed.
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${SCRIPT_DIR}"
export XALOQI_LICENSE_SKIP=1

QUICK=0
for arg in "$@"; do
    case "$arg" in
        --quick)
            QUICK=1
            ;;
        -h|--help)
            echo "Usage: bash run_python_tests.sh [--quick]"
            echo "  --quick   Skip basic_ecu's robustness campaign (12 files, ~110s)."
            exit 0
            ;;
        *)
            echo "Unknown option: $arg" >&2
            exit 2
            ;;
    esac
done

ROBUSTNESS_IGNORE=(
    --ignore=test_robustness_A_codegen.py
    --ignore=test_robustness_B_protocol.py
    --ignore=test_robustness_C_security.py
    --ignore=test_robustness_D_customer_journey.py
    --ignore=test_robustness_E_data_integrity.py
    --ignore=test_robustness_F_codegen_limits.py
    --ignore=test_robustness_G_resilience.py
    --ignore=test_robustness_H_protocol_precision.py
    --ignore=test_robustness_I_nrc_wdbi_sa.py
    --ignore=test_robustness_J_sovd_cda.py
    --ignore=test_robustness_K_error_quality.py
    --ignore=test_robustness_L_codegen_output_fidelity.py
)

TOTAL_SUITES=0
FAIL_SUITES=0
ENV_SUITES=0
declare -a SUMMARY_LINES

# run_suite LABEL DIR [extra pytest args...]
run_suite() {
    local label="$1" dir="$2"
    shift 2
    TOTAL_SUITES=$((TOTAL_SUITES + 1))

    local out rc tail_line
    out="$(cd "$dir" && python3 -m pytest . -q "$@" 2>&1)"
    rc=$?
    tail_line="$(printf '%s\n' "$out" | tail -n 1)"

    local passed failed
    passed="$(printf '%s\n' "$tail_line" | grep -oE '[0-9]+ passed' | grep -oE '^[0-9]+' || echo 0)"
    failed="$(printf '%s\n' "$tail_line" | grep -oE '[0-9]+ failed' | grep -oE '^[0-9]+' || echo 0)"
    local errored=0
    printf '%s\n' "$tail_line" | grep -qE '[0-9]+ error' && errored=1

    if [ "$rc" -ne 0 ] || [ "$failed" -gt 0 ] || [ "$errored" -eq 1 ]; then
        FAIL_SUITES=$((FAIL_SUITES + 1))
        SUMMARY_LINES+=("FAIL  ${label}: ${tail_line}")
        echo "===== ${label}: FAIL ====="
        printf '%s\n' "$out"
        echo "===================================================="
    elif [ "$passed" -eq 0 ]; then
        # Collected fine, zero failures, but nothing actually ran/passed —
        # an environment gap (missing optional dependency / firmware
        # binary / license module), not a verified pass.
        ENV_SUITES=$((ENV_SUITES + 1))
        SUMMARY_LINES+=("[ENV] ${label}: ${tail_line}")
    else
        SUMMARY_LINES+=("PASS  ${label}: ${tail_line}")
    fi
}

echo "======================================================================"
echo " Xaloqi EDS — canonical Python test suite (issue #150)"
echo " XALOQI_LICENSE_SKIP=1"
echo "======================================================================"

START_TS=$(date +%s)

echo
echo "--- tests/ (repo-level suite) ---"
run_suite "tests/" "${ROOT}/tests"

for dir in "${ROOT}"/examples/*/generated/tests; do
    [ -d "$dir" ] || continue
    example_name="$(basename "$(dirname "$(dirname "$dir")")")"
    echo
    echo "--- examples/${example_name}/generated/tests ---"
    if [ "$QUICK" -eq 1 ] && [ -f "${dir}/test_robustness_A_codegen.py" ]; then
        run_suite "examples/${example_name}" "$dir" "${ROBUSTNESS_IGNORE[@]}"
    else
        run_suite "examples/${example_name}" "$dir"
    fi
done

END_TS=$(date +%s)

echo
echo "======================================================================"
echo " Summary (${TOTAL_SUITES} suites, $((END_TS - START_TS))s)"
echo "======================================================================"
for line in "${SUMMARY_LINES[@]}"; do
    echo " $line"
done
echo "----------------------------------------------------------------------"
echo " ${FAIL_SUITES} failed, ${ENV_SUITES} environment-incomplete, $((TOTAL_SUITES - FAIL_SUITES - ENV_SUITES)) passed clean"
echo "======================================================================"

if [ "$FAIL_SUITES" -gt 0 ]; then
    exit 1
fi
exit 0
