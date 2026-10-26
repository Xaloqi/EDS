#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# run-builds.sh: build every row of builds.tsv with CONFIG_COMPILER_WARNINGS_AS_ERRORS=y.
# Environment: WS (west workspace root, holds zephyr/ and EDS/), OUT (artifact dir),
#              ZEPHYR_SDK_INSTALL_DIR. Run from anywhere.
# Does not stop at the first failure: every row is attempted and gets one line in $OUT/results.tsv
#   name<TAB>PASS|FAIL<TAB>detail
# and the first failure's name, first error line and log tail go to $OUT/first-failure.txt and
# tail.txt (for the tracking issue). The verdict is check-results.py's, not this script's exit code.
set -uo pipefail
: "${WS:?}" "${OUT:?}" "${ZEPHYR_SDK_INSTALL_DIR:?}"
HERE=$(cd "$(dirname "$0")" && pwd)
EDS=$WS/EDS
mkdir -p "$OUT"; : > "$OUT/results.tsv"; : > "$OUT/sweep-dirs.txt"
cd "$WS"

native_board() {  # <app dir relative to EDS>: the example's own native_sim files, else the generic ones
  local d=$EDS/$1/boards/native_sim conf overlay
  [ -f "$d/native_sim.conf" ] && conf=$d/native_sim.conf || conf=$EDS/boards/native_sim/native_sim.conf
  [ -f "$d/native_sim.overlay" ] && overlay=$d/native_sim.overlay || overlay=$EDS/boards/native_sim/native_sim.overlay
  echo "-DEXTRA_CONF_FILE=$conf -DDTC_OVERLAY_FILE=$overlay"
}

fail() {  # <name> <detail> <log>
  echo -e "$1\tFAIL\t$2" >> "$OUT/results.tsv"
  echo "::error::$1: $2"
  if [ ! -s "$OUT/first-failure.txt" ]; then
    { echo "$1: $2"; { grep -a -m1 -E "error:|Error " "$3" || grep -a -m1 "FAILED" "$3"; } 2>/dev/null | cut -c1-300; } > "$OUT/first-failure.txt"
    tail -n 30 "$3" > "$OUT/tail.txt" 2>/dev/null || true
  fi
}

while IFS=$'\t' read -r name kind app board sweep extra; do
  case $name in ''|'#'*) continue;; esac
  log=$OUT/$name.log; bdir=$WS/build/$name
  args=${extra//@EDS@/$EDS}
  args=${args//@NATIVE_BOARD@/$(native_board "$app")}
  if [[ $args == *@PRJ_NO_CANARIES@* ]]; then
    grep -v -E '^CONFIG_(EDS_REQUIRE_)?STACK_CANARIES=' "$EDS/$app/prj.conf" > "$OUT/prj-$name.conf"
    args=${args//@PRJ_NO_CANARIES@/$OUT/prj-$name.conf}
  fi
  extra_kc=""
  [ "$board" != native_sim ] && extra_kc="-DCONFIG_NEWLIB_LIBC=n -DCONFIG_EDS_REQUIRE_NEWLIB=n"
  if [ "$board" = native_sim ]; then export ZEPHYR_TOOLCHAIN_VARIANT=host; else export ZEPHYR_TOOLCHAIN_VARIANT=zephyr; fi
  echo "::group::$name ($app on $board)"
  # shellcheck disable=SC2086
  west build -p always -d "$bdir" -b "$board" "$EDS/$app" -- \
    -DDIAG_SKIP_CODEGEN=ON -DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y $extra_kc $args > "$log" 2>&1
  rc=$?
  tail -n 5 "$log"; echo "::endgroup::"
  if [ $rc -ne 0 ]; then fail "$name" "west build exited $rc" "$log"; continue; fi
  cp "$bdir/zephyr/.config" "$OUT/config-$name" 2>/dev/null || true
  if [ "$board" = native_sim ]; then img=$bdir/zephyr/zephyr.exe; else img=$bdir/zephyr/zephyr.elf; fi
  [ -f "$img" ] || { fail "$name" "build exited 0 but ${img##*/} does not exist" "$log"; continue; }
  grep -qx 'CONFIG_COMPILER_WARNINGS_AS_ERRORS=y' "$bdir/zephyr/.config" \
    || { fail "$name" "CONFIG_COMPILER_WARNINGS_AS_ERRORS=y is not in the final .config" "$log"; continue; }
  if [ "$kind" = test ]; then
    timeout 120 "$img" > "$OUT/$name.run.log" 2>&1 || true
    if grep -q 'PASS - test_handler_registered_and_sets_bus_off' "$OUT/$name.run.log" \
       && grep -q 'PROJECT EXECUTION SUCCESSFUL' "$OUT/$name.run.log"; then :
    else fail "$name" "built, but the test did not print its PASS line and the ztest success banner" "$OUT/$name.run.log"; continue; fi
  fi
  [ "$sweep" = y ] && echo "$name" >> "$OUT/sweep-dirs.txt"
  echo -e "$name\tPASS\t$kind" >> "$OUT/results.tsv"
done < "$HERE/builds.tsv"
exit 0
