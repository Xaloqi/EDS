#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# install-sdk.sh <zephyr tree> <install parent dir> <out dir>
# Installs the Zephyr SDK that THIS Zephyr tree asks for (zephyr/SDK_VERSION): the minimal bundle
# plus the arm and aarch64 toolchains, each verified against the release's own sha256.sum. A file
# the sum does not list is an error. Prints the ZEPHYR_SDK_INSTALL_DIR line to stdout.
set -euo pipefail
ZEPHYR=$1; DEST=$2; OUT=$3
V=$(tr -d '[:space:]' < "$ZEPHYR/SDK_VERSION")
[ -n "$V" ] || { echo "::error::no SDK_VERSION in the Zephyr tree" >&2; exit 1; }
case $V in 0.*) echo "::error::Zephyr SDK $V: this job installs the SDK 1.x tarball layout only (zephyr_rev older than 4.4 is not supported)" >&2; exit 1;; esac
BASE=https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v$V
MIN=zephyr-sdk-${V}_linux-x86_64_minimal.tar.xz
TCS=(toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz toolchain_gnu_linux-x86_64_aarch64-zephyr-elf.tar.xz)
echo "Zephyr SDK required by this tree: $V" >&2
echo "$V" > "$OUT/sdk.txt"
cd "$DEST"
wget -q "$BASE/sha256.sum" "$BASE/$MIN" $(for t in "${TCS[@]}"; do echo "$BASE/$t"; done)
: > wanted.sum
for f in "$MIN" "${TCS[@]}"; do
  grep -E " \*?$f\$" sha256.sum >> wanted.sum || { echo "::error::sha256.sum of SDK v$V does not list $f" >&2; exit 1; }
done
[ "$(wc -l < wanted.sum)" = "$((1 + ${#TCS[@]}))" ] || { echo "::error::sha256.sum lists a file more than once" >&2; cat wanted.sum >&2; exit 1; }
sha256sum -c wanted.sum >&2
tar xf "$MIN"
mkdir -p "zephyr-sdk-$V/gnu"
for t in "${TCS[@]}"; do tar xf "$t" -C "zephyr-sdk-$V/gnu"; done
rm -f ./*.tar.xz
echo "$DEST/zephyr-sdk-$V"
