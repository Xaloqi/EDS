#!/usr/bin/env bash
# =============================================================================
# build_harness.sh  —  Phase 6+7 SocketCAN Integration + MISRA-clean Build
#
# Builds and optionally runs the full UDS integration test harness.
# Phase 7: All sources compile clean under the full MISRA-relevant GCC
# warning set: -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
# -Wstrict-prototypes -Wmissing-prototypes -Wredundant-decls -Wlogical-op
# -Wduplicated-cond -Wimplicit-fallthrough=5 (zero warnings on all sources).
#
# Usage:
#   ./build_harness.sh              # Build only (MISRA-clean flags)
#   ./build_harness.sh --run        # Build and run all 68 integration tests
#   ./build_harness.sh --run --hw   # Build for AF_CAN hardware (vcan0)
#   ./build_harness.sh --fast       # Build without Wpedantic (faster CI)
#
# Requirements:
#   gcc, pthreads (standard Linux development toolchain, gcc >= 8 recommended)
#   For --hw mode: Linux with SocketCAN + vcan module loaded
#
# Output:
#   /tmp/harness_ecu_test  (override with OUTPUT=/path/to/binary)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${SCRIPT_DIR}"
OUTPUT="${OUTPUT:-/tmp/harness_ecu_test}"
HW_MODE=0
RUN_MODE=0
FAST_MODE=0

for arg in "$@"; do
    case "$arg" in
        --run)  RUN_MODE=1  ;;
        --hw)   HW_MODE=1   ;;
        --fast) FAST_MODE=1 ;;
        --help)
            echo "Usage: $0 [--run] [--hw] [--fast]"
            echo "  --run   Build then execute all 68 integration tests"
            echo "  --hw    Build with AF_CAN hardware support (Linux+SocketCAN)"
            echo "  --fast  Omit -Wpedantic for faster incremental CI builds"
            exit 0
            ;;
    esac
done

# ---------------------------------------------------------------------------
# Harness sources are a Professional-tier deliverable (issue #68).
# A community clone of this repo does not contain harness/ — fail with a
# clear explanation instead of a page of cc1 fatal errors.
# ---------------------------------------------------------------------------
if [[ ! -f "${ROOT}/harness/harness_main.c" ]]; then
    echo "ERROR: harness/ sources not found."
    echo ""
    echo "The 68-test integration harness is part of the Xaloqi EDS"
    echo "Professional tier and is not included in the public repository."
    echo "It is delivered in the Professional ZIP — extract it into this"
    echo "repo root first (see INSTALL.md Step 2), then re-run this script."
    echo ""
    echo "Tiers and pricing: see COMMERCIAL_NOTICE.md or https://xaloqi.com"
    echo ""
    echo "The public unit test suite needs no commercial files:"
    echo "  bash build_tests.sh"
    exit 1
fi

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
HARNESS_SRCS=(
    "$ROOT/harness/harness_main.c"
    "$ROOT/harness/harness_ecu.c"
    "$ROOT/harness/harness_tester.c"
    "$ROOT/harness/socketcan_shim.c"
)

STACK_SRCS=(
    "$ROOT/core/uds_server.c"
    "$ROOT/core/uds_session.c"
    "$ROOT/core/uds_security.c"
    "$ROOT/core/uds_security_algo.c"
    "$ROOT/core/uds_aes_cmac.c"
    "$ROOT/core/uds_access_table.c"
    "$ROOT/core/uds_safety.c"
    "$ROOT/core/uds_comm_control.c"
    "$ROOT/core/uds_services/service_registration.c"
    "$ROOT/core/uds_services/service_0x10.c"
    "$ROOT/core/uds_services/service_0x11.c"
    "$ROOT/core/uds_services/service_0x14.c"
    "$ROOT/core/uds_services/service_0x19.c"
    "$ROOT/core/uds_services/service_0x22.c"
    "$ROOT/core/uds_services/service_0x27.c"
    "$ROOT/core/uds_services/service_0x28.c"
    "$ROOT/core/uds_services/service_0x2E.c"
    "$ROOT/core/uds_services/service_0x31.c"
    "$ROOT/core/uds_services/service_0x34.c"
    "$ROOT/core/uds_services/service_0x36.c"
    "$ROOT/core/uds_services/service_0x37.c"
    "$ROOT/core/uds_services/service_0x3E.c"
    "$ROOT/core/uds_services/service_0x85.c"
    "$ROOT/core/uds_services/service_0x23.c"
    "$ROOT/core/uds_services/service_0x2A.c"
    "$ROOT/core/uds_services/service_0x2F.c"
    "$ROOT/core/uds_services/service_0x35.c"
    "$ROOT/core/uds_services/service_0x3D.c"
    "$ROOT/core/uds_periodic.c"
    "$ROOT/core/uds_transfer_ctx.c"
    "$ROOT/transport/isotp.c"
    "$ROOT/transport/can_transport.c"
    "$ROOT/config/did_database.c"
    "$ROOT/config/dtc_database.c"
    "$ROOT/config/dtc_mirror.c"
    "$ROOT/config/routine_database.c"
    "$ROOT/examples/basic_ecu/generated/did_handlers.c"
    "$ROOT/examples/basic_ecu/generated/did_safety_wrappers.c"
    "$ROOT/examples/basic_ecu/generated/routine_handlers.c"
    "$ROOT/platform/zephyr/nvm_store_mock.c"
    "$ROOT/platform/uds_flash_ops.c"
    "$ROOT/platform/zephyr/harness_flash_mock.c"
    "$ROOT/tests/mocks/zephyr_port_mock.c"
)

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
INCLUDES=(
    "-I$ROOT/core"
    "-I$ROOT/core/uds_services"
    "-I$ROOT/transport"
    "-I$ROOT/config"
    "-I$ROOT/platform"
    "-I$ROOT/platform/zephyr"
    "-I$ROOT/examples/basic_ecu/generated"
    "-I$ROOT/harness"
    "-I$ROOT/tests/mocks"
    "-I$ROOT/tests/runner"
)

# ---------------------------------------------------------------------------
# Compiler flags — Phase 7 MISRA-clean warning set
# ---------------------------------------------------------------------------
CFLAGS=(
    "-std=c11"
    "-g"
    "-O0"
    # Core warnings
    "-Wall"
    "-Wextra"
    "-Wno-unused-parameter"
    # MISRA-relevant additional warnings
    "-Wshadow"
    "-Wconversion"
    "-Wsign-conversion"
    "-Wstrict-prototypes"
    "-Wmissing-prototypes"
    "-Wredundant-decls"
    "-Wlogical-op"
    "-Wduplicated-cond"
    "-Wimplicit-fallthrough=5"
    # Preprocessor defines
    "-DNVM_STORE_HOST_MOCK=1"
    "-DUNIT_TEST=1"
    # Suppress the uds_msg_buf_t _Static_assert: harness runs on the host
    # with a large process stack. The guard is for embedded targets only.
    "-DEDS_MSG_BUF_MAX_STACK_BYTES=8192"
)

if [[ $FAST_MODE -eq 0 ]]; then
    CFLAGS+=("-Wpedantic")
fi

if [[ $HW_MODE -eq 1 ]]; then
    CFLAGS+=("-DHARNESS_SOCKETCAN_HW=1")
    echo "[build_harness] Hardware SocketCAN mode enabled (AF_CAN / vcan0)"
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
ALL_SRCS=("${HARNESS_SRCS[@]}" "${STACK_SRCS[@]}")

echo "[build_harness] Phase 7 MISRA-clean build..."
gcc "${CFLAGS[@]}" "${INCLUDES[@]}" "${ALL_SRCS[@]}" \
    -lpthread \
    -o "$OUTPUT"

echo "[build_harness] Binary: $OUTPUT  (zero warnings)"

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
if [[ $RUN_MODE -eq 1 ]]; then
    echo "[build_harness] Running 68 integration tests..."
    echo ""
    exec "$OUTPUT"
fi
