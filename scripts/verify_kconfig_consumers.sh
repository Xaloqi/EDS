#!/usr/bin/env bash
# [EDS#455] Every `config` symbol in EDS's Kconfig must be read by the code.
#
# Eight of thirteen symbols once were not: prj.conf settings for ISO-TP timing,
# padding, S3 and the SecurityAccess lockout were silently ignored while the
# Kconfig header claimed "the Kconfig value wins". This fails the gate if a
# symbol has no CONFIG_<NAME> reference in core/, transport/, platform/,
# config/, cmake/ or the example applications (examples/*/src). `configdefault` lines set Zephyr's own symbols and are
# not checked.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail=0
while read -r sym; do
    if ! grep -rqE "CONFIG_${sym}\b" "${ROOT}/core" "${ROOT}/transport" \
            "${ROOT}/platform" "${ROOT}/config" "${ROOT}/cmake" "${ROOT}/examples" \
            --include='*.c' --include='*.h' --include='*.cmake' 2>/dev/null; then
        echo "  FAIL: Kconfig symbol ${sym} is read by nothing (EDS#455)"
        fail=1
    fi
done < <(grep -oE '^[[:space:]]*config[[:space:]]+[A-Z0-9_]+' "${ROOT}/Kconfig" | awk '{print $2}')
if [[ ${fail} -eq 0 ]]; then
    echo "  PASS: every Kconfig symbol has a consumer (EDS#455)"
fi
exit ${fail}
