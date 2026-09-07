#!/usr/bin/env bash
# =============================================================================
# Xaloqi EDS
# run_python_tests.sh  (project root)
#
# PURPOSE: Canonical entrypoint for the whole Python test suite (issue #150).
#          Mirrors build_tests.sh's role for the C suite: one command, run
#          from anywhere in the repo, that exercises everything correctly
#          scoped and prints a clear pass/blocked/fail summary.
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
# EXECUTION-TRUTH SEMANTICS (ADR-005) — three outcomes, not two:
#   Case level:   EXECUTED-PASS / EXECUTED-FAIL / NOT-EXECUTED (skipped,
#                 deselected, or never collected).
#   Suite level, derived, precedence FAIL > BLOCKED > PASS:
#     FAIL     any real test failure, a collection error, or an abnormal
#              exit pytest couldn't explain with a normal summary line.
#     BLOCKED  no failure, but the suite has no declared floor, or its
#              executed count is below the declared floor. Printed
#              explicitly as "BLOCKED:", never a silent/bare PASS.
#     PASS     no failure, and executed count is at or above the
#              declared floor.
#   A suite with no declared floor CANNOT report PASS — it reports
#   BLOCKED, however many cases happen to run (ADR-005 rule 3).
#
# PROFILE-AWARE FLOORS (v1.14.0 Phase 4a) — ADR-005 rule 3 defines a floor
#   as the number of cases a suite executes *when its prerequisites are
#   present*, and prerequisites differ by tier. Two named profiles, chosen
#   automatically by whether harness/harness_main.c exists (the same test
#   ci.yml's harness-tests job uses):
#     developer     harness/ absent — what CI and a community clone see.
#     professional  harness/ present — the commercial-tier checkout.
#   Both floor tables below were derived by running this exact script,
#   `XALOQI_LICENSE_SKIP=1`, under `bash --noprofile --norc -eo pipefail`,
#   once per profile, and reading off the real executed counts (issue
#   #234/#230 scoping, v1.14.0 Phase 4). They are declared constants, not
#   re-derived on every run — re-deriving from whatever happens to execute
#   locally would rebuild the exact false-pass defect this script exists
#   to remove (see the ENV-classifier history at issue #213 and the
#   "292 vs 439" robustness-campaign story in .github/workflows/ci.yml).
#
#   tests/ keeps NO floor in either profile (always `unknown` → always
#   BLOCKED). It executes 0 cases in both profiles today (needs
#   xaloqi-tester's firmware binary for the DoIP integration tests — the
#   ECU binary this checkout never builds — and tools/_license.py for the
#   license tests), and it additionally has a real bug tracked at #260. A
#   floor of 0 would let it report PASS having executed nothing — exactly
#   what ADR-005 rule 1 forbids — so it stays BLOCKED unconditionally
#   until #260 is fixed and a real floor can be observed.
#
#   BLOCKED is this same mechanism (built for #213, then named ENV) under
#   its one spelling repo-wide (ADR-005): here, and in
#   `tests/test_doip_integration.py` and `tests/test_license_expiry.py`.
#   Renamed because BLOCKED also covers tier-gated artifacts and absent
#   credentials, not only environment gaps, and reads clearly to external
#   evaluators. Look for "BLOCKED" in the underlying pytest skip reasons to
#   grep the exact cause of any one case.
#
# EDS_QUALIFICATION_RUN=1 (ADR-005 rule 5):
#   Unset (normal/local/public run) — BLOCKED is permitted, printed
#   explicitly, and never fails the run; it is never counted toward any
#   claim regardless.
#   Set to exactly "1" — BLOCKED becomes a hard failure. Use this in a
#   release-qualification context, where every suite's prerequisites are
#   expected to be genuinely present.
#   FAIL always fails the run either way.
#
# MACHINE-READABLE OUTPUT (ADR-005 rule 6):
#   Every run overwrites test-outcomes.json at the repo root (gitignored —
#   a generated build artifact, never committed) with each suite's
#   {outcome, executed, passed, failed, not_executed, floor} plus a
#   top-level aggregate, for downstream consumption (the release gate,
#   check_release_docs.py) without re-parsing this script's prose output.
#
# USAGE:
#   bash run_python_tests.sh              # full suite (all examples + tests/)
#   bash run_python_tests.sh --quick       # skip basic_ecu's 12-file, ~110s
#                                          # robustness campaign (still runs
#                                          # in its own dedicated CI job)
#
# EXIT CODES:
#   0  No suite FAILed, and either no suite is BLOCKED or
#      EDS_QUALIFICATION_RUN is unset (BLOCKED permitted — ADR-005 rule 5).
#   1  At least one suite FAILed, OR at least one suite is BLOCKED and
#      EDS_QUALIFICATION_RUN=1.
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${SCRIPT_DIR}"
export XALOQI_LICENSE_SKIP=1

# -----------------------------------------------------------------------
# Profile selection (ADR-005 rule 3 / v1.14.0 Phase 4a) — see the header
# comment above for how these numbers were derived and why tests/ has no
# floor in either profile.
# -----------------------------------------------------------------------
if [ -f "${ROOT}/harness/harness_main.c" ]; then
    PROFILE="professional"
    PROFILE_REASON="harness/harness_main.c present"
else
    PROFILE="developer"
    PROFILE_REASON="harness/harness_main.c absent (Professional-tier deliverable, #68)"
fi

declare -A FLOOR_DEVELOPER=(
    ["examples/ardep_ecu"]=312
    ["examples/basic_ecu"]=426
    ["examples/basic_ecu_doip"]=99
    ["examples/basic_ecu_doip_freertos"]=99
    ["examples/basic_ecu_freertos"]=99
    ["examples/bms_ecu"]=228
    ["examples/motor_controller_ecu"]=263
    ["examples/robot_joint_controller_ecu"]=142
    ["examples/safeboot_ecu"]=80
    ["examples/sensor_ecu"]=105
    ["examples/sensor_ecu_freertos"]=105
)
declare -A FLOOR_PROFESSIONAL=(
    ["examples/ardep_ecu"]=450
    ["examples/basic_ecu"]=482
    ["examples/basic_ecu_doip"]=155
    ["examples/basic_ecu_doip_freertos"]=155
    ["examples/basic_ecu_freertos"]=155
    ["examples/bms_ecu"]=331
    ["examples/motor_controller_ecu"]=379
    ["examples/robot_joint_controller_ecu"]=215
    ["examples/safeboot_ecu"]=130
    ["examples/sensor_ecu"]=165
    ["examples/sensor_ecu_freertos"]=165
)

# floor_for LABEL — echoes the declared floor for the active profile, or
# the literal string "unknown" when LABEL has none (tests/, always, by
# design — see the header comment).
floor_for() {
    local label="$1" value=""
    if [ "$PROFILE" = "professional" ]; then
        value="${FLOOR_PROFESSIONAL[$label]:-}"
    else
        value="${FLOOR_DEVELOPER[$label]:-}"
    fi
    if [ -n "$value" ]; then
        echo "$value"
    else
        echo "unknown"
    fi
}

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
BLOCKED_SUITES=0
TOTAL_EXECUTED=0
TOTAL_NOT_EXECUTED=0
TOTAL_COLLECTED=0
declare -a SUMMARY_LINES
declare -a SUITE_JSON

# run_suite LABEL DIR FLOOR [extra pytest args...]
#   FLOOR is a non-negative integer (the declared minimum executed count
#   this suite must reach to be entitled to PASS — ADR-005 rule 3), or the
#   literal string "unknown" when this checkout cannot determine that
#   number (see the header comment above). "unknown" always reports
#   BLOCKED, never PASS, no matter how many cases execute.
run_suite() {
    local label="$1" dir="$2" floor="$3"
    shift 3
    TOTAL_SUITES=$((TOTAL_SUITES + 1))

    local out rc tail_line had_errexit=0
    case "$-" in *e*) had_errexit=1 ;; esac
    # A real test failure makes pytest exit non-zero, and a plain
    # (non-`local`-combined) `var="$(cmd)"` assignment propagates that as
    # the assignment statement's own exit status — under a caller that has
    # errexit active (GitHub Actions' default `bash -e` for `run:` steps;
    # this script's own `set -uo pipefail` deliberately omits -e, but does
    # not un-inherit it if a caller already enabled it), that would abort
    # the whole script right here on the first FAIL, before the FAIL
    # banner, the summary, or test-outcomes.json (ADR-005 rule 6) ever get
    # written. Disable errexit only for this substitution, and restore the
    # caller's exact original setting afterward (not unconditionally
    # `set -e`, which would turn errexit on for the rest of the script even
    # when the caller never asked for it).
    set +e
    out="$(cd "$dir" && python3 -m pytest . -q "$@" 2>&1)"
    rc=$?
    [ "$had_errexit" -eq 1 ] && set -e
    tail_line="$(printf '%s\n' "$out" | tail -n 1)"

    local passed failed skipped deselected errored
    passed="$(printf '%s\n' "$tail_line" | grep -oE '[0-9]+ passed' | grep -oE '^[0-9]+' || echo 0)"
    failed="$(printf '%s\n' "$tail_line" | grep -oE '[0-9]+ failed' | grep -oE '^[0-9]+' || echo 0)"
    skipped="$(printf '%s\n' "$tail_line" | grep -oE '[0-9]+ skipped' | grep -oE '^[0-9]+' || echo 0)"
    deselected="$(printf '%s\n' "$tail_line" | grep -oE '[0-9]+ deselected' | grep -oE '^[0-9]+' || echo 0)"
    errored=0
    printf '%s\n' "$tail_line" | grep -qE '[0-9]+ error' && errored=1

    local executed=$((passed + failed))
    local not_executed=$((skipped + deselected))

    # rc outside {0, 5} with no failed/error signal in the tail line at all
    # is an abnormal exit (e.g. pytest itself crashed before it could print
    # a summary) — treat it as FAIL rather than silently falling through to
    # BLOCKED/PASS logic that assumes a well-formed tail line. rc 5 = "no
    # tests collected", which a module-level pytest.importorskip()/
    # pytest.skip(allow_module_level=True) hits even though every case
    # validly skipped (issue #213) — not an abnormal exit.
    local abnormal=0
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 5 ] && [ "$failed" -eq 0 ] && [ "$errored" -eq 0 ]; then
        abnormal=1
    fi

    TOTAL_EXECUTED=$((TOTAL_EXECUTED + executed))
    TOTAL_NOT_EXECUTED=$((TOTAL_NOT_EXECUTED + not_executed))
    TOTAL_COLLECTED=$((TOTAL_COLLECTED + executed + not_executed))

    local outcome
    if [ "$failed" -gt 0 ] || [ "$errored" -eq 1 ] || [ "$abnormal" -eq 1 ]; then
        outcome="FAIL"
    elif [ "$floor" = "unknown" ]; then
        outcome="BLOCKED"
    elif [ "$executed" -ge "$floor" ]; then
        outcome="PASS"
    else
        outcome="BLOCKED"
    fi

    case "$outcome" in
        FAIL)
            FAIL_SUITES=$((FAIL_SUITES + 1))
            SUMMARY_LINES+=("FAIL     ${label}: ${executed} executed, ${not_executed} not executed — ${tail_line}")
            echo "===== ${label}: FAIL ====="
            printf '%s\n' "$out"
            echo "===================================================="
            ;;
        BLOCKED)
            BLOCKED_SUITES=$((BLOCKED_SUITES + 1))
            if [ "$floor" = "unknown" ]; then
                SUMMARY_LINES+=("BLOCKED  ${label}: ${executed} executed, ${not_executed} not executed — no declared floor: ${tail_line}")
                echo "BLOCKED: ${label} has no declared floor (ADR-005 rule 3) — its prerequisites"
                echo "         are not verifiable as complete in this checkout, so it cannot report"
                echo "         PASS regardless of how many cases ran. ${executed} executed,"
                echo "         ${not_executed} not executed. Never counted toward a claim."
            else
                SUMMARY_LINES+=("BLOCKED  ${label}: ${executed} executed, ${not_executed} not executed — below declared floor of ${floor}: ${tail_line}")
                echo "BLOCKED: ${label} executed ${executed} of its declared floor of ${floor} —"
                echo "         below floor (ADR-005 rule 3). Never counted toward a claim."
            fi
            ;;
        PASS)
            SUMMARY_LINES+=("PASS     ${label}: ${executed} executed (floor ${floor}), ${not_executed} not executed — ${tail_line}")
            ;;
    esac

    local floor_json
    if [ "$floor" = "unknown" ]; then
        floor_json="null"
    else
        floor_json="$floor"
    fi
    SUITE_JSON+=("$(printf '    {"suite": "%s", "outcome": "%s", "executed": %d, "passed": %d, "failed": %d, "not_executed": %d, "floor": %s}' \
        "$label" "$outcome" "$executed" "$passed" "$failed" "$not_executed" "$floor_json")")
}

echo "======================================================================"
echo " Xaloqi EDS — canonical Python test suite (issue #150)"
echo " XALOQI_LICENSE_SKIP=1  EDS_QUALIFICATION_RUN=${EDS_QUALIFICATION_RUN:-<unset>}"
echo " Profile: ${PROFILE} (${PROFILE_REASON}) — ADR-005 rule 3 floors"
echo "======================================================================"

START_TS=$(date +%s)

echo
echo "--- tests/ (repo-level suite) ---"
# tests/ has no floor in either profile — see the header comment and #260.
run_suite "tests/" "${ROOT}/tests" unknown

for dir in "${ROOT}"/examples/*/generated/tests; do
    [ -d "$dir" ] || continue
    example_name="$(basename "$(dirname "$(dirname "$dir")")")"
    label="examples/${example_name}"
    floor="$(floor_for "$label")"
    echo
    echo "--- ${label}/generated/tests ---"
    if [ "$QUICK" -eq 1 ] && [ -f "${dir}/test_robustness_A_codegen.py" ]; then
        run_suite "$label" "$dir" "$floor" "${ROBUSTNESS_IGNORE[@]}"
    else
        run_suite "$label" "$dir" "$floor"
    fi
done

END_TS=$(date +%s)

echo
echo "======================================================================"
echo " Summary (${TOTAL_SUITES} suites, $((END_TS - START_TS))s) — profile: ${PROFILE} (${PROFILE_REASON})"
echo "======================================================================"
for line in "${SUMMARY_LINES[@]}"; do
    echo " $line"
done
echo "----------------------------------------------------------------------"
echo " ${FAIL_SUITES} failed, ${BLOCKED_SUITES} blocked, $((TOTAL_SUITES - FAIL_SUITES - BLOCKED_SUITES)) passed"
echo " ${TOTAL_EXECUTED} of ${TOTAL_COLLECTED} cases executed across all suites (${TOTAL_NOT_EXECUTED} not executed)"
echo "======================================================================"

# ---------------------------------------------------------------------------
# test-outcomes.json (ADR-005 rule 6) — machine-readable summary, gitignored
# build artifact. Written on every run, pass or fail, so downstream
# consumers (release gate, check_release_docs.py) always have the latest.
# ---------------------------------------------------------------------------
{
    echo "{"
    echo "  \"profile\": \"${PROFILE}\","
    echo "  \"profile_reason\": \"${PROFILE_REASON}\","
    echo "  \"suites\": ["
    joined=""
    for entry in "${SUITE_JSON[@]}"; do
        if [ -n "$joined" ]; then
            joined="${joined},"$'\n'"${entry}"
        else
            joined="${entry}"
        fi
    done
    printf '%s\n' "$joined"
    echo "  ],"
    echo "  \"totals\": {"
    echo "    \"suites_total\": ${TOTAL_SUITES},"
    echo "    \"suites_pass\": $((TOTAL_SUITES - FAIL_SUITES - BLOCKED_SUITES)),"
    echo "    \"suites_fail\": ${FAIL_SUITES},"
    echo "    \"suites_blocked\": ${BLOCKED_SUITES},"
    echo "    \"executed\": ${TOTAL_EXECUTED},"
    echo "    \"not_executed\": ${TOTAL_NOT_EXECUTED},"
    echo "    \"collected\": ${TOTAL_COLLECTED}"
    echo "  }"
    echo "}"
} > "${ROOT}/test-outcomes.json"
echo " Wrote ${ROOT}/test-outcomes.json"
echo "======================================================================"

if [ "$FAIL_SUITES" -gt 0 ]; then
    echo
    echo "FAIL: ${FAIL_SUITES} suite(s) reported a real test failure or collection error."
    exit 1
fi

if [ "$BLOCKED_SUITES" -gt 0 ]; then
    echo
    if [ "${EDS_QUALIFICATION_RUN:-}" = "1" ]; then
        echo "ERROR: EDS_QUALIFICATION_RUN=1 — BLOCKED is a hard failure in a qualification"
        echo "       context (ADR-005 rule 5). ${BLOCKED_SUITES} suite(s) reported BLOCKED."
        echo "       Refusing to pass."
        exit 1
    fi
    echo "Public/local run — BLOCKED is permitted here (ADR-005 rule 5); ${BLOCKED_SUITES}"
    echo "suite(s) reported BLOCKED above and are never counted toward a claim. Exiting 0."
    exit 0
fi

exit 0
