# =============================================================================
# Xaloqi EDS
# examples/ardep_ecu/CMakeLists.txt
#
# PURPOSE: Stand-alone Zephyr build for the ARDEP I/O Controller example.
#
# TARGET BOARD: ardep (Mercedes-Benz ARDEP in the ARDEP Zephyr workspace)
#
# BUILDING:
#   # Standard (requires ARDEP workspace — see docs/ARDEP_UPGRADE_GUIDE.md):
#   cd <ardep-workspace>
#   west build -b ardep <path-to-eds>/examples/ardep_ecu
#
#   # With explicit overlays (if board overlays not auto-discovered):
#   west build -b ardep <path-to-eds>/examples/ardep_ecu \
#     -- -DDIAG_SKIP_CODEGEN=ON \
#     -DEXTRA_CONF_FILE=<path-to-eds>/examples/ardep_ecu/boards/ardep/ardep.conf \
#     -DDTC_OVERLAY_FILE=<path-to-eds>/examples/ardep_ecu/boards/ardep/ardep.overlay
#
#   # Simulation build (no hardware required, for testing):
#   west build -b native_sim <path-to-eds>/examples/ardep_ecu \
#     -- -DDIAG_SKIP_CODEGEN=ON \
#     -DEXTRA_CONF_FILE=<path-to-eds>/boards/native_sim/native_sim.conf \
#     -DDTC_OVERLAY_FILE=<path-to-eds>/boards/native_sim/native_sim.overlay
#
# CODE GENERATION:
#   Generated files are placed in examples/ardep_ecu/generated/ to keep
#   them separate from the basic_ecu generated/ directory.
#   Run manually:
#     python3 tools/codegen.py \
#       --config  examples/ardep_ecu/diagnostics_config.yaml \
#       --out     examples/ardep_ecu/generated/ \
#       --safety-wrappers --asil-level B --test-gen
#
# SPDX-License-Identifier: Apache-2.0
# =============================================================================

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ardep_ecu
    VERSION     1.0.0
    LANGUAGES   C
    DESCRIPTION "ARDEP I/O Controller — Xaloqi EDS example"
)

set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
target_compile_options(app PRIVATE
    -Wall -Wextra -Wshadow -Wdouble-promotion -Wundef
    -Wno-unused-parameter -Wno-sign-conversion
    -fstack-usage
)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
set(DIAG_ROOT           "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(DIAG_CONFIG_YAML    "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics_config.yaml")
set(DIAG_CODEGEN_SCRIPT "${DIAG_ROOT}/tools/codegen.py")

# Generated files live in this example's own generated/ directory
# (separate from basic_ecu's generated/ to allow independent configs)
set(DIAG_GENERATED_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/generated")
set(CODEGEN_SENTINEL    "${DIAG_GENERATED_DIR}/generated_config.h")

# ---------------------------------------------------------------------------
# Code generation
# ---------------------------------------------------------------------------
option(DIAG_SKIP_CODEGEN
    "Skip auto-running tools/codegen.py (use pre-existing generated/ files)"
    OFF
)

if(NOT DIAG_SKIP_CODEGEN)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_FOUND)
        message(FATAL_ERROR
            "[ARDEP] Python3 not found. Run codegen manually:\n"
            "  python3 tools/codegen.py \\\n"
            "    --config  examples/ardep_ecu/diagnostics_config.yaml \\\n"
            "    --out     examples/ardep_ecu/generated/ \\\n"
            "    --safety-wrappers --asil-level B\n"
            "Then re-configure with -DDIAG_SKIP_CODEGEN=ON\n"
        )
    endif()

    file(GLOB DIAG_TEMPLATES "${DIAG_ROOT}/tools/templates/*.j2")

    add_custom_command(
        OUTPUT   "${CODEGEN_SENTINEL}"
        COMMAND  "${Python3_EXECUTABLE}"
                 "${DIAG_CODEGEN_SCRIPT}"
                 --config "${DIAG_CONFIG_YAML}"
                 --out    "${DIAG_GENERATED_DIR}"
                 --safety-wrappers
                 --asil-level B
                 --no-manifest
        DEPENDS
            "${DIAG_CONFIG_YAML}"
            "${DIAG_CODEGEN_SCRIPT}"
            ${DIAG_TEMPLATES}
        COMMENT  "[ARDEP] Generating diagnostics sources..."
        VERBATIM
    )
    add_custom_target(ardep_codegen DEPENDS "${CODEGEN_SENTINEL}")
else()
    message(STATUS "[ARDEP] DIAG_SKIP_CODEGEN=ON — using ${DIAG_GENERATED_DIR}")
endif()

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
target_include_directories(app PRIVATE
    ${DIAG_ROOT}/core
    ${DIAG_ROOT}/core/uds_services
    ${DIAG_ROOT}/transport
    ${DIAG_ROOT}/config
    ${DIAG_ROOT}/platform
    ${DIAG_ROOT}/platform/zephyr
    ${DIAG_GENERATED_DIR}
)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
target_sources(app PRIVATE
    # Application
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c

    # UDS core
    ${DIAG_ROOT}/core/uds_server.c
    ${DIAG_ROOT}/core/uds_session.c
    ${DIAG_ROOT}/core/uds_security.c
    ${DIAG_ROOT}/core/uds_safety.c
    ${DIAG_ROOT}/core/uds_security_algo.c
    ${DIAG_ROOT}/core/uds_aes_cmac.c
    ${DIAG_ROOT}/core/uds_access_table.c
    ${DIAG_ROOT}/core/uds_comm_control.c
    ${DIAG_ROOT}/core/uds_session_stats.c
    ${DIAG_ROOT}/core/uds_security_nvm.c

    # UDS services
    ${DIAG_ROOT}/core/uds_services/service_registration.c
    ${DIAG_ROOT}/core/uds_services/service_0x10.c
    ${DIAG_ROOT}/core/uds_services/service_0x11.c
    ${DIAG_ROOT}/core/uds_services/service_0x14.c
    ${DIAG_ROOT}/core/uds_services/service_0x19.c
    ${DIAG_ROOT}/core/uds_services/service_0x22.c
    ${DIAG_ROOT}/core/uds_services/service_0x27.c
    ${DIAG_ROOT}/core/uds_services/service_0x28.c
    ${DIAG_ROOT}/core/uds_services/service_0x31.c
    ${DIAG_ROOT}/core/uds_services/service_0x2E.c
    ${DIAG_ROOT}/core/uds_services/service_0x3E.c
    ${DIAG_ROOT}/core/uds_services/service_0x85.c
    # DFU services — added with Phase 7 firmware download support.
    # service_registration.c references all three in g_uds_service_table[];
    # omitting these causes link failure: undefined reference to uds_service_0x34_handler.
    ${DIAG_ROOT}/core/uds_services/service_0x34.c   # RequestDownload
    ${DIAG_ROOT}/core/uds_services/service_0x35.c   # RequestUpload
    ${DIAG_ROOT}/core/uds_services/service_0x36.c   # TransferData
    ${DIAG_ROOT}/core/uds_services/service_0x37.c   # RequestTransferExit
    ${DIAG_ROOT}/core/uds_transfer_ctx.c             # DFU transfer state machine
    ${DIAG_ROOT}/platform/uds_flash_ops.c            # Flash ops singleton

    # ISO-TP / CAN transport
    ${DIAG_ROOT}/transport/isotp.c
    ${DIAG_ROOT}/transport/can_transport.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_can.c  # moved from transport/ — phase-housekeeping

    # Configuration databases
    ${DIAG_ROOT}/config/did_database.c
    ${DIAG_ROOT}/config/dtc_database.c
    ${DIAG_ROOT}/config/routine_database.c
    ${DIAG_ROOT}/config/dtc_mirror.c

    # Zephyr platform abstraction
    ${DIAG_ROOT}/platform/zephyr/zephyr_port.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_mutex.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_timer.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_wdt.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_platform_api.c  # eds_platform_* neutral interface

    # Generated sources
    ${DIAG_GENERATED_DIR}/did_handlers.c
    ${DIAG_GENERATED_DIR}/uds_init.c
    ${DIAG_GENERATED_DIR}/did_safety_wrappers.c
    ${DIAG_GENERATED_DIR}/routine_handlers.c
)

# NVM store: real Zephyr NVS on hardware, RAM mock on native_sim
if(CONFIG_BOARD_NATIVE_SIM)
    target_sources(app PRIVATE ${DIAG_ROOT}/platform/zephyr/nvm_store_mock.c)
else()
    target_sources(app PRIVATE ${DIAG_ROOT}/platform/zephyr/nvm_store.c)
endif()

if(NOT DIAG_SKIP_CODEGEN AND TARGET ardep_codegen)
    add_dependencies(app ardep_codegen)
endif()

# Suppress the uds_msg_buf_t _Static_assert for all Zephyr example builds.
# The Zephyr task stacks for these examples (4096-6144 bytes) are large enough
# to hold the struct if needed, but all EDS stack buffers are module-level
# statics — the assert would fire as a false positive on every compilation unit.
# Zephyr's native_sim gets this via the CMakeLists.txt native_sim block;
# hardware targets need it here.
target_compile_definitions(app PRIVATE EDS_MSG_BUF_MAX_STACK_BYTES=8192)
