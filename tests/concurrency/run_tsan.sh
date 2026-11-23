#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Xaloqi
#
# Host ThreadSanitizer tests (tests/concurrency/). Each test runs RUNS times;
# any data race report fails it. native_sim runs one Zephyr thread at a time
# and cannot show these races; real pthreads under TSan can.
# Not wired into CI yet (EDS#197 January plan, J1).
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
     -I$ROOT/tests/runner -I$ROOT/tests/mocks -I$ROOT/transport/doip"
CFLAGS="-std=gnu11 -g -O1 -fsanitize=thread -DUNIT_TEST=1 -DEDS_TEST_HOOKS=1 -DNVM_STORE_HOST_MOCK=1 \
        -DZTEST_HOST_SHIM=1 -DEDS_MSG_BUF_MAX_STACK_BYTES=8192 -include $ROOT/tests/runner/ztest_shim.h"

fail=0
for src in "$ROOT"/tests/concurrency/tsan_*.c; do
    name=$(basename "$src" .c)
    # shellcheck disable=SC2086
    if ! gcc $CFLAGS $INC "$src" $SRCS -o "$OUT/$name" -lpthread 2> "$OUT/$name.build.log"; then
        echo "BUILD FAIL $name"; cat "$OUT/$name.build.log"; fail=1; continue
    fi
    for run in $(seq 1 "$RUNS"); do
        TSAN_OPTIONS=halt_on_error=0 "$OUT/$name" > "$OUT/$name.$run.log" 2>&1
        rc=$?
        races=$(grep -c 'WARNING: ThreadSanitizer: data race' "$OUT/$name.$run.log")
        if [ "$rc" -ne 0 ] || [ "$races" -ne 0 ]; then
            echo "FAIL $name run $run: $races data race(s), exit $rc"
            grep -A3 'WARNING: ThreadSanitizer: data race' "$OUT/$name.$run.log" \
                | grep -E '^\s+#0' | sed "s#$ROOT/##; s/ (.*//" | sort | uniq -c
            fail=1
        else
            echo "PASS $name run $run: 0 data races ($(tail -1 "$OUT/$name.$run.log"))"
        fi
    done
done
exit $fail
