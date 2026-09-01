#!/usr/bin/env bash
# =============================================================================
# Xaloqi EDS
# scripts/verify_sanitizer_gate.sh
#
# PURPOSE: The pass/fail gate for CI's 'sanitizers' job (issue #151).
#
#          `bash build_tests.sh --sanitize` builds and runs every host unit
#          test module under AddressSanitizer + UndefinedBehaviorSanitizer.
#          This script decides whether that run may be called green.
#
# WHY THIS IS NOT `if build_tests.sh; then echo ok; fi`
#
#          Knowledge lessons/run-013: a CI gate must assert POSITIVE success,
#          not merely the absence of failure. EDS shipped a green
#          "Integration Tests" job that executed zero tests from the initial
#          public release onward, because its gate was
#          `grep -q "failed" && exit 1 || true` and "136 skipped" contains no
#          "failed". Every one of these would pass an exit-code-only gate on
#          a run with no sanitizer coverage whatsoever:
#
#            - the --sanitize flag silently stops being applied (arg-parsing
#              regression, renamed flag, a `case` arm that stopped matching);
#            - the compiler on the runner has no sanitizer runtime, so the
#              instrumentation is a no-op;
#            - UBSan finds a real violation but, being recoverable by
#              default, prints one line and lets the process exit 0;
#            - the test loop builds nothing, or builds binaries that execute
#              zero assertions and still exit 0.
#
#          So this script asserts, positively and in order:
#            1. The project's sanitizer flags can be recovered from
#               build_tests.sh at all (if not, everything below is vacuous).
#            2. Those exact flags really do catch a planted ASan violation
#               AND a planted UBSan violation on this runner, right now.
#               The gate proves it can fail before it is allowed to pass.
#            3. The test binaries that were actually run carry ASan and UBSan
#               instrumentation (checked in the ELF, not taken on trust).
#            4. Every test module was built and run, none aborted, and the
#               suite executed a real minimum number of assertions with zero
#               failures.
#            5. No sanitizer diagnostic appears anywhere in the run log.
#
# USAGE:
#   bash build_tests.sh --sanitize --keep-bin 2>&1 | tee sanitizer_run.log
#   bash scripts/verify_sanitizer_gate.sh sanitizer_run.log [build_test_host]
#
# EXIT CODES:
#   0  The sanitizer run genuinely happened and was genuinely clean.
#   1  Any assertion above failed.
#
# SAFETY CONTEXT : CI infrastructure — not safety-assessed.
# SPDX-License-Identifier: GPL-2.0-only
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

LOG="${1:-}"
BIN_DIR="${2:-${ROOT}/build_test_host}"

if [[ -z "${LOG}" || ! -f "${LOG}" ]]; then
    echo "usage: bash scripts/verify_sanitizer_gate.sh <run-log> [bin-dir]"
    exit 1
fi

# A module that ran zero assertions is the exact failure run-013 describes,
# so the floor is deliberately close to the real number (894 at the time of
# writing) rather than a token "> 0". It is a floor, not an equality, so
# adding test cases never breaks the gate; removing most of them does.
MIN_CASES=800
# Guards against the module list itself being wiped: the expected module
# count is derived from the repo (so it never goes stale), but a derived
# expectation of 0 or 3 must not be allowed to satisfy the gate.
MIN_MODULES=40

FAILURES=0
note_fail() {
    echo "  FAIL: $*"
    FAILURES=$((FAILURES + 1))
}

echo "================================================================"
echo "  [#151] Sanitizer gate — positive verification"
echo "================================================================"
echo ""

# ---------------------------------------------------------------------------
# 1. Recover the project's sanitizer flags from build_tests.sh.
#
# Derived rather than duplicated: a second hand-maintained copy of the flag
# list is precisely the "two copies of the same fact drift apart" failure
# this repo keeps paying for. If the derivation itself breaks, that is a hard
# failure — silently falling back to "no flags" would make step 2 vacuous.
# ---------------------------------------------------------------------------
mapfile -t SAN_FLAGS < <(
    sed -n '/^if \[\[ \$SANITIZE -eq 1 \]\]; then/,/^fi$/p' "${ROOT}/build_tests.sh" \
        | grep -oE '"-f[^"]+"' | tr -d '"'
)

echo "--- 1. sanitizer flags recovered from build_tests.sh ---"
if [[ ${#SAN_FLAGS[@]} -eq 0 ]]; then
    note_fail "could not recover any -f flags from build_tests.sh's --sanitize block."
    echo "        Everything below would be vacuous; refusing to continue."
    exit 1
fi
printf '  %s\n' "${SAN_FLAGS[@]}"

if ! printf '%s\n' "${SAN_FLAGS[@]}" | grep -q -- '-fsanitize=address,undefined'; then
    note_fail "-fsanitize=address,undefined is not among the recovered flags."
fi
if ! printf '%s\n' "${SAN_FLAGS[@]}" | grep -q -- '-fno-sanitize-recover=all'; then
    note_fail "-fno-sanitize-recover=all is not among the recovered flags — UBSan" \
              "would print a diagnostic and let the process exit 0."
fi
echo ""

# ---------------------------------------------------------------------------
# 2. Canaries: prove the gate can fail, on this runner, with these flags.
#
# Two deliberately-broken programs, one per sanitizer, compiled with the
# flags recovered above. Each must be caught. If a canary is NOT caught, the
# sanitizer coverage on this runner is not real and the clean run below
# proves nothing.
#
# The canaries live here, generated into a temp dir, rather than as committed
# .c files: a file containing a deliberate buffer overrun must never be
# reachable by any other build in this repo.
# ---------------------------------------------------------------------------
CANARY_DIR="$(mktemp -d)"
trap 'rm -rf "${CANARY_DIR}"' EXIT

cat > "${CANARY_DIR}/asan_canary.c" <<'CANARY_EOF'
/* Deliberate heap-buffer-overflow. Must be caught by AddressSanitizer. */
#include <stdlib.h>
#include <string.h>
int main(void)
{
    volatile size_t n = 5U;          /* volatile: not foldable at compile time */
    char *p = (char *)malloc(4U);
    if (p == NULL) { return 2; }
    memset(p, 'x', n);               /* one byte past the end of a 4-byte block */
    free(p);
    return 0;
}
CANARY_EOF

cat > "${CANARY_DIR}/ubsan_canary.c" <<'CANARY_EOF'
/* Deliberate signed integer overflow. Must be caught by UBSan. */
#include <limits.h>
int main(void)
{
    volatile int a = INT_MAX;
    volatile int b = 1;
    return (int)(a + b);             /* signed overflow: undefined behaviour */
}
CANARY_EOF

check_canary() {
    local name="$1" expect="$2"
    local src="${CANARY_DIR}/${name}_canary.c"
    local bin="${CANARY_DIR}/${name}_canary"
    local out rc

    if ! gcc -std=c11 -g -O0 "${SAN_FLAGS[@]}" "${src}" -o "${bin}" 2>"${CANARY_DIR}/${name}.build"; then
        note_fail "${name} canary failed to COMPILE with the project's sanitizer flags:"
        sed 's/^/        /' "${CANARY_DIR}/${name}.build"
        return
    fi

    out=$("${bin}" 2>&1) && rc=0 || rc=$?

    if [[ ${rc} -eq 0 ]]; then
        note_fail "${name} canary exited 0 — a deliberate violation was NOT caught." \
                  "Sanitizer coverage on this runner is not real."
        return
    fi
    if ! grep -qE "${expect}" <<< "${out}"; then
        note_fail "${name} canary failed, but not with the expected diagnostic" \
                  "(/${expect}/). Got:"
        sed 's/^/        /' <<< "${out}" | head -5
        return
    fi
    echo "  OK: ${name} canary was caught — $(grep -oE "${expect}" <<< "${out}" | head -1)"
}

echo "--- 2. canaries (the gate must be able to fail before it may pass) ---"
check_canary asan  'AddressSanitizer: heap-buffer-overflow'
check_canary ubsan 'runtime error: signed integer overflow'
echo ""

# ---------------------------------------------------------------------------
# 3. The binaries that actually ran must carry the instrumentation.
#
# The canaries prove the toolchain works; this proves the flags reached the
# test binaries themselves. Without it, --sanitize could quietly stop being
# applied to the suite while the canaries kept passing.
# ---------------------------------------------------------------------------
echo "--- 3. instrumentation present in the test binaries ---"
EXPECTED_MODULES=$(find "${ROOT}/tests/unit_runnable" -maxdepth 1 -name 'test_*.c' | wc -l)
if [[ "${EXPECTED_MODULES}" -lt "${MIN_MODULES}" ]]; then
    note_fail "only ${EXPECTED_MODULES} module source(s) found in tests/unit_runnable/" \
              "(expected at least ${MIN_MODULES}) — the expectation itself is unusable."
fi

if [[ ! -d "${BIN_DIR}" ]]; then
    note_fail "binary directory ${BIN_DIR} does not exist — was --keep-bin passed?"
else
    mapfile -t BINARIES < <(find "${BIN_DIR}" -maxdepth 1 -type f -executable -name 'test_*' | LC_ALL=C sort)
    echo "  test binaries found: ${#BINARIES[@]} (expected ${EXPECTED_MODULES})"
    if [[ ${#BINARIES[@]} -ne ${EXPECTED_MODULES} ]]; then
        note_fail "expected ${EXPECTED_MODULES} test binaries, found ${#BINARIES[@]}."
    fi

    uninstrumented=0
    for b in "${BINARIES[@]}"; do
        syms=$(nm -D "${b}" 2>/dev/null || true)
        if ! grep -q '__asan' <<< "${syms}" || ! grep -q '__ubsan' <<< "${syms}"; then
            echo "        not instrumented: $(basename "${b}")"
            uninstrumented=$((uninstrumented + 1))
        fi
    done
    if [[ ${uninstrumented} -ne 0 ]]; then
        note_fail "${uninstrumented} test binary/binaries carry no ASan+UBSan symbols."
    elif [[ ${#BINARIES[@]} -gt 0 ]]; then
        echo "  OK: all ${#BINARIES[@]} test binaries carry both __asan* and __ubsan* symbols"
    fi
fi
echo ""

# ---------------------------------------------------------------------------
# 4. The run itself: assert what happened, not that nothing went wrong.
# ---------------------------------------------------------------------------
echo "--- 4. the run executed the whole suite ---"

if ! grep -q '=== Build: ASan + UBSan' "${LOG}"; then
    note_fail "the log does not state that this was a sanitizer build —" \
              "build_tests.sh did not take its --sanitize branch."
fi

num_from() {  # num_from <regex-with-one-group>
    sed -nE "s/.*$1.*/\1/p" "${LOG}" | tail -1
}
passed=$(num_from 'Unit Test Summary: ([0-9]+) passed')
failed=$(num_from 'Unit Test Summary: [0-9]+ passed, ([0-9]+) failed')
cases_run=$(num_from 'Unit Test Cases: ([0-9]+) run')
cases_failed=$(num_from 'Unit Test Cases: [0-9]+ run, ([0-9]+) failed')

if [[ -z "${passed}" || -z "${failed}" || -z "${cases_run}" || -z "${cases_failed}" ]]; then
    note_fail "could not parse the summary lines out of ${LOG}." \
              "The run did not reach its own summary."
else
    echo "  modules: ${passed} passed, ${failed} failed (expected ${EXPECTED_MODULES} passed, 0 failed)"
    echo "  cases:   ${cases_run} run, ${cases_failed} failed (floor ${MIN_CASES} run, 0 failed)"

    [[ "${failed}" -eq 0 ]]        || note_fail "${failed} module(s) failed."
    [[ "${cases_failed}" -eq 0 ]]  || note_fail "${cases_failed} test case(s) failed."
    [[ "${passed}" -eq "${EXPECTED_MODULES}" ]] \
        || note_fail "${passed} module(s) passed but tests/unit_runnable/ holds ${EXPECTED_MODULES}."
    [[ "${cases_run}" -ge "${MIN_CASES}" ]] \
        || note_fail "only ${cases_run} test case(s) executed (floor ${MIN_CASES}) —" \
                     "the suite ran, but barely; something is being skipped."
fi
echo ""

# ---------------------------------------------------------------------------
# 5. No sanitizer diagnostic anywhere in the log.
#
# Secondary to the assertions above, deliberately: on its own this is exactly
# the "absence of a bad string" check run-013 warns about, and an empty log
# would satisfy it. It is here to catch a diagnostic that somehow did not
# take the process down.
# ---------------------------------------------------------------------------
echo "--- 5. no sanitizer diagnostics in the run log ---"
diag=$(grep -cE 'AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error:|SUMMARY: .*Sanitizer' "${LOG}" || true)
if [[ "${diag}" -ne 0 ]]; then
    note_fail "${diag} sanitizer diagnostic line(s) in ${LOG}:"
    grep -nE 'AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error:|SUMMARY: .*Sanitizer' \
        "${LOG}" | head -20 | sed 's/^/        /'
else
    echo "  OK: none"
fi
echo ""

echo "================================================================"
if [[ ${FAILURES} -ne 0 ]]; then
    echo "  FAIL: sanitizer gate — ${FAILURES} assertion(s) failed."
    echo "================================================================"
    exit 1
fi
echo "  PASS: ${EXPECTED_MODULES} modules / ${cases_run} cases ran under"
echo "        ASan + UBSan with 0 failures and 0 sanitizer diagnostics,"
echo "        on binaries verified to be instrumented, with both canaries"
echo "        confirming the gate can still fail."
echo "================================================================"
exit 0
