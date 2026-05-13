# =============================================================================
# Xaloqi EDS
# examples/basic_ecu_doip/CMakeLists.txt
#
# PURPOSE: Stand-alone Zephyr build for the DoIP example ECU (v1.6.0).
#
# Mirrors examples/basic_ecu/CMakeLists.txt exactly, with three additions:
#   1. Networking include path (-I transport/doip, -I platform/zephyr)
#   2. DoIP transport sources (zephyr_lwip.c, platform_doip.c, doip_server.c)
#   3. CAN sources OMITTED (DoIP-only transport: no isotp.c, no zephyr_can.c)
#
# BUILDING:
#   west build -b native_sim examples/basic_ecu_doip
#
# CI target (native_sim loopback, no hardware required):
#   west build -b native_sim examples/basic_ecu_doip -- \
#       -DEXTRA_CONF_FILE=boards/native_sim/native_sim_doip.conf
#
# SAFETY CONTEXT : Example application — not safety-assessed.
# SPDX-License-Identifier: Apache-2.0
# =============================================================================

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(basic_ecu_doip
    VERSION     1.6.0
    LANGUAGES   C
    DESCRIPTION "BasicECU DoIP example — Xaloqi EDS v1.6.0"
)

set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

target_compile_options(app PRIVATE
    -Wall -Wextra -Wshadow -Wdouble-promotion -Wundef
    -Wno-unused-parameter -Wno-sign-conversion
    -fstack-usage
)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
set(DIAG_ROOT         "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(DIAG_CONFIG_YAML  "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics_config.yaml")
set(DIAG_CODEGEN_SCRIPT "${DIAG_ROOT}/tools/codegen.py")
set(DIAG_GENERATED_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/generated")
set(CODEGEN_SENTINEL    "${DIAG_GENERATED_DIR}/safety_config.h")

option(DIAG_SKIP_CODEGEN "Skip auto-running codegen.py" OFF)

if(NOT DIAG_SKIP_CODEGEN)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_FOUND)
        message(FATAL_ERROR "[DIAG] Python3 not found — cannot run codegen.")
    endif()
    file(GLOB DIAG_TEMPLATES "${DIAG_ROOT}/tools/templates/*.j2")
    add_custom_command(
        OUTPUT   "${CODEGEN_SENTINEL}"
        COMMAND  "${Python3_EXECUTABLE}"
                 "${DIAG_CODEGEN_SCRIPT}"
                 --config "${DIAG_CONFIG_YAML}"
                 --out    "${DIAG_GENERATED_DIR}"
                 --safety-wrappers --asil-level B --no-manifest
        DEPENDS  "${DIAG_CONFIG_YAML}" "${DIAG_CODEGEN_SCRIPT}" ${DIAG_TEMPLATES}
        COMMENT  "[DIAG] Generating DoIP ECU sources from diagnostics_config.yaml..."
        VERBATIM
    )
    add_custom_target(run_codegen DEPENDS "${CODEGEN_SENTINEL}")
endif()

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
target_include_directories(app PRIVATE
    ${DIAG_ROOT}/core
    ${DIAG_ROOT}/core/uds_services
    ${DIAG_ROOT}/transport
    ${DIAG_ROOT}/transport/doip          # doip_server.h, zephyr_lwip.h
    ${DIAG_ROOT}/config
    ${DIAG_ROOT}/platform
    ${DIAG_ROOT}/platform/zephyr         # platform_doip.h, zephyr_port.h, etc.
    ${DIAG_GENERATED_DIR}
)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
target_sources(app PRIVATE

    # ── Application entry point ──────────────────────────────────────────
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c

    # ── UDS core ─────────────────────────────────────────────────────────
    ${DIAG_ROOT}/core/uds_server.c
    ${DIAG_ROOT}/core/uds_session.c
    ${DIAG_ROOT}/core/uds_security.c
    ${DIAG_ROOT}/core/uds_safety.c
    ${DIAG_ROOT}/core/uds_security_algo.c
    ${DIAG_ROOT}/core/uds_access_table.c
    ${DIAG_ROOT}/core/uds_comm_control.c
    ${DIAG_ROOT}/core/uds_session_stats.c
    ${DIAG_ROOT}/core/uds_security_nvm.c
    ${DIAG_ROOT}/core/uds_aes_cmac.c

    # ── UDS service handlers ──────────────────────────────────────────────
    ${DIAG_ROOT}/core/uds_services/service_registration.c
    ${DIAG_ROOT}/core/uds_services/service_0x10.c
    ${DIAG_ROOT}/core/uds_services/service_0x11.c
    ${DIAG_ROOT}/core/uds_services/service_0x14.c
    ${DIAG_ROOT}/core/uds_services/service_0x19.c
    ${DIAG_ROOT}/core/uds_services/service_0x22.c
    ${DIAG_ROOT}/core/uds_services/service_0x27.c
    ${DIAG_ROOT}/core/uds_services/service_0x28.c
    ${DIAG_ROOT}/core/uds_services/service_0x2E.c
    ${DIAG_ROOT}/core/uds_services/service_0x31.c
    ${DIAG_ROOT}/core/uds_services/service_0x3E.c
    ${DIAG_ROOT}/core/uds_services/service_0x85.c
    ${DIAG_ROOT}/core/uds_services/service_0x34.c
    ${DIAG_ROOT}/core/uds_services/service_0x36.c
    ${DIAG_ROOT}/core/uds_services/service_0x37.c
    ${DIAG_ROOT}/core/uds_transfer_ctx.c
    ${DIAG_ROOT}/platform/uds_flash_ops.c

    # ── DoIP transport (Zephyr binding) ───────────────────────────────────
    ${DIAG_ROOT}/transport/doip/doip_server.c
    ${DIAG_ROOT}/transport/doip/zephyr_lwip.c
    ${DIAG_ROOT}/platform/zephyr/platform_doip.c

    # ── NOTE: No isotp.c / can_transport.c / zephyr_can.c ────────────────
    # This is a DoIP-only build (transport: doip in diagnostics_config.yaml).
    # For transport: both, add those sources alongside the DoIP sources.

    # ── Configuration databases ───────────────────────────────────────────
    ${DIAG_ROOT}/config/did_database.c
    ${DIAG_ROOT}/config/dtc_database.c
    ${DIAG_ROOT}/config/routine_database.c
    ${DIAG_ROOT}/config/dtc_mirror.c

    # ── Zephyr platform abstraction ───────────────────────────────────────
    # Minimal set for DoIP-only: no zephyr_can.c, no zephyr_timer.c,
    # no zephyr_wdt.c (those are CAN-path dependencies).
    ${DIAG_ROOT}/platform/zephyr/zephyr_platform_api.c

    # ── Generated sources ─────────────────────────────────────────────────
    ${DIAG_GENERATED_DIR}/did_handlers.c
    ${DIAG_GENERATED_DIR}/uds_init.c
    ${DIAG_GENERATED_DIR}/did_safety_wrappers.c
    ${DIAG_GENERATED_DIR}/routine_handlers.c
)

# NVM mock for native_sim (no NVS flash on simulator)
if(CONFIG_BOARD_NATIVE_SIM)
    target_sources(app PRIVATE ${DIAG_ROOT}/platform/zephyr/nvm_store_mock.c)
else()
    target_sources(app PRIVATE ${DIAG_ROOT}/platform/zephyr/nvm_store.c)
endif()

if(NOT DIAG_SKIP_CODEGEN AND TARGET run_codegen)
    add_dependencies(app run_codegen)
endif()

target_compile_definitions(app PRIVATE
    EDS_MSG_BUF_MAX_STACK_BYTES=8192
    EDS_DOIP_ONLY_BUILD=1
)
