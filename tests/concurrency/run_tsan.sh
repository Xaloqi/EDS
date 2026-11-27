#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Xaloqi
#
# Host ThreadSanitizer tests (tests/concurrency/). Each tsan_*.c test runs RUNS
# times. native_sim runs one Zephyr thread at a time and cannot show these
# races; real pthreads under TSan can. CI: the concurrency-tsan job (EDS#197 J1).
#
# Two kinds of test:
#   - expected pass (the default): any data race report, or a non-zero exit,
#     fails it;
#   - XFAIL, marked "TSAN_EXPECT: XFAIL" in the source: a known race that
#     must still show. Every run must report data races > 0 AND print
#     "request_count=<n> expected=<N>" with n < N. A run that does not is an
#     XPASS and fails the job: an XFAIL that stops failing proves nothing,
#     so the fix must drop the marker in the same change.
# The script also fails if it ran fewer tests than there are tsan_*.c files,
# or none at all: a test that silently did not run is not a pass.
#
# Last line: "TSAN SUMMARY: files=F ran=R pass=P xfail=X fail=K".
#
# Usage: tests/concurrency/run_tsan.sh [RUNS]      (gcc with -fsanitize=thread)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
RUNS=${1:-3}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# Same stack sources as build_tests.sh, minus its test runner.
SRCS=$(sed -n '/^STACK_SRCS=(/,/^)/p' "$ROOT/build_tests.sh" | grep -o '\${ROOT}/[^" ]*\.c' \
       | sed "s#\${ROOT}#$ROOT#" | grep -v test_main.c)
INC="-I$ROOT/core -I$ROOT/core/uds_services -I$ROOT/transport -I$ROOT/config -I$ROOT/platform \
     -I$ROOT/platform/zephyr -I$ROOT/examples/basic_ecu/generated -I$ROOT/project/unity \
     -I$ROOT/tests/runner -I$ROOT/tests/mocks -I$ROOT/transport/doip -I$ROOT/tests/concurrency"
CFLAGS="-std=gnu11 -g -O1 -fsanitize=thread -DUNIT_TEST=1 -DEDS_TEST_HOOKS=1 -DNVM_STORE_HOST_MOCK=1 \
        -DZTEST_HOST_SHIM=1 -DEDS_MSG_BUF_MAX_STACK_BYTES=8192 -include $ROOT/tests/runner/ztest_shim.h"

files=0; ran=0; pass=0; xfail=0; fail=0
for src in "$ROOT"/tests/concurrency/tsan_*.c; do
    [ -f "$src" ] || continue
    files=$((files + 1))
    name=$(basename "$src" .c)
    kind=pass
    grep -q 'TSAN_EXPECT: XFAIL' "$src" && kind=xfail
    # shellcheck disable=SC2086
    if ! gcc $CFLAGS $INC "$src" $SRCS -o "$OUT/$name" -lpthread 2> "$OUT/$name.build.log"; then
        echo "BUILD FAIL $name"; cat "$OUT/$name.build.log"; fail=$((fail + 1)); continue
    fi
    ran=$((ran + 1))
    test_ok=1
    for run in $(seq 1 "$RUNS"); do
        TSAN_OPTIONS=halt_on_error=0 "$OUT/$name" > "$OUT/$name.$run.log" 2>&1
        rc=$?
        races=$(grep -c 'WARNING: ThreadSanitizer: data race' "$OUT/$name.$run.log")
        count=$(sed -n 's/.*request_count=\([0-9]*\) expected=\([0-9]*\).*/\1 \2/p' "$OUT/$name.$run.log" | tail -1)
        if [ "$kind" = xfail ]; then
            set -- $count
            # TSan exits 66 after reporting; anything else non-zero is a crash.
            if [ "$#" -eq 2 ] && [ "$races" -gt 0 ] && [ "$1" -lt "$2" ] \
               && { [ "$rc" -eq 0 ] || [ "$rc" -eq 66 ]; }; then
                echo "XFAIL $name run $run (expected): $races data race(s), request_count $1 of $2"
            else
                echo "XPASS $name run $run: $races data race(s), request_count ${1:-?} of ${2:-?}, exit $rc;" \
                     "the known race no longer shows: fix it for real and drop TSAN_EXPECT: XFAIL"
                test_ok=0
            fi
        elif [ "$rc" -ne 0 ] || [ "$races" -ne 0 ]; then
            echo "FAIL $name run $run: $races data race(s), exit $rc"
            grep -A3 'WARNING: ThreadSanitizer: data race' "$OUT/$name.$run.log" \
                | grep -E '^\s+#0' | sed "s#$ROOT/##; s/ (.*//" | sort | uniq -c
            test_ok=0
        else
            echo "PASS $name run $run: 0 data races ($(tail -1 "$OUT/$name.$run.log"))"
        fi
    done
    if [ "$test_ok" -eq 0 ]; then
        fail=$((fail + 1))
    elif [ "$kind" = xfail ]; then
        xfail=$((xfail + 1))
    else
        pass=$((pass + 1))
    fi
done

echo "TSAN SUMMARY: files=$files ran=$ran pass=$pass xfail=$xfail fail=$fail"
if [ "$files" -eq 0 ] || [ "$ran" -lt "$files" ]; then
    echo "FAIL: ran $ran of $files tsan_*.c test(s); a test that did not run is not a pass"
    exit 1
fi
[ "$fail" -eq 0 ]
