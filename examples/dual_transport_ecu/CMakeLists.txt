# =============================================================================
# Xaloqi EDS
# examples/dual_transport_ecu/CMakeLists.txt
#
# PURPOSE: [EDS#197] CAN (ISO-TP, classic or FD) and DoIP on one ECU, both
#          producers of the diagnostic executor. The sources of basic_ecu
#          (CAN) and basic_ecu_doip (DoIP) together, without
#          EDS_DOIP_ONLY_BUILD.
#
# BUILDING (see README.md for the board files):
#   west build -b native_sim examples/dual_transport_ecu -- -DDIAG_SKIP_CODEGEN=ON \
#       -DEXTRA_CONF_FILE=boards/native_sim/native_sim.conf \
#       -DDTC_OVERLAY_FILE=boards/native_sim/native_sim.overlay
#   CAN FD: add fd.conf to EXTRA_CONF_FILE (CONFIG_CAN_FD_MODE=y).
#
# SAFETY CONTEXT : Example application — not safety-assessed.
# SPDX-License-Identifier: Apache-2.0
# =============================================================================

cmake_minimum_required(VERSION 3.20.0)

# [#414] EDS Kconfig comes from this tree. Must precede find_package(Zephyr).
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/eds_zephyr_module.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(dual_transport_ecu
    VERSION     1.0.0
    LANGUAGES   C
    DESCRIPTION "CAN + DoIP on the diagnostic executor — Xaloqi EDS (EDS#197)"
)

set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

# [#515] -Wundef (MISRA 20.9) applies to EDS code only.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/eds_third_party_headers.cmake)
eds_third_party_headers_are_system(app)

target_compile_options(app PRIVATE
    -Wall -Wextra -Wshadow -Wdouble-promotion -Wundef
    -Wno-unused-parameter -Wno-sign-conversion
    -fstack-usage
)

# ---------------------------------------------------------------------------
# Paths and codegen
# ---------------------------------------------------------------------------
set(DIAG_ROOT           "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(DIAG_CONFIG_YAML    "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics_config.yaml")
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
        COMMENT  "[DIAG] Generating dual-transport ECU sources from diagnostics_config.yaml..."
        VERBATIM
    )
    add_custom_target(run_codegen DEPENDS "${CODEGEN_SENTINEL}")
    add_dependencies(app run_codegen)
endif()

target_include_directories(app PRIVATE
    ${DIAG_ROOT}/core
    ${DIAG_ROOT}/core/uds_services
    ${DIAG_ROOT}/transport
    ${DIAG_ROOT}/transport/doip
    ${DIAG_ROOT}/config
    ${DIAG_ROOT}/platform
    ${DIAG_ROOT}/platform/zephyr
    ${DIAG_GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
include(${DIAG_ROOT}/cmake/eds_service_sources.cmake)   # EDS_SERVICE_SRCS (uds_exec.c too)

target_sources(app PRIVATE
    # Application: main() and the CAN producer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/can_producer.c

    # UDS core and services
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
    ${EDS_SERVICE_SRCS}
    ${DIAG_ROOT}/core/uds_transfer_ctx.c
    ${DIAG_ROOT}/platform/uds_flash_ops.c
    ${DIAG_ROOT}/platform/uds_image_policy.c

    # CAN: ISO-TP and the Zephyr CAN binding
    ${DIAG_ROOT}/transport/isotp.c
    ${DIAG_ROOT}/transport/can_transport.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_can.c

    # DoIP: the server and its Zephyr binding
    ${DIAG_ROOT}/transport/doip/doip_server.c
    ${DIAG_ROOT}/transport/doip/zephyr_lwip.c
    ${DIAG_ROOT}/platform/zephyr/platform_doip.c

    # Configuration databases
    ${DIAG_ROOT}/config/did_database.c
    ${DIAG_ROOT}/config/dtc_database.c
    ${DIAG_ROOT}/config/routine_database.c
    ${DIAG_ROOT}/config/dtc_mirror.c

    # Zephyr platform: CAN port, the executor, the watchdog
    ${DIAG_ROOT}/platform/zephyr/zephyr_port.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_mutex.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_timer.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_platform_api.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_exec.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_wdt.c

    # Generated
    ${DIAG_GENERATED_DIR}/did_handlers.c
    ${DIAG_GENERATED_DIR}/uds_init.c
    ${DIAG_GENERATED_DIR}/did_safety_wrappers.c
    ${DIAG_GENERATED_DIR}/routine_handlers.c
)

# NVM store (EDS#408): the RAM mock on native_sim; on hardware the board's
# diag_nvs partition (pass boards/<board>/<board>.overlay as DTC_OVERLAY_FILE).
include(${DIAG_ROOT}/cmake/eds_nvm_backend.cmake)
eds_select_nvm_backend(app ${DIAG_ROOT})

target_compile_definitions(app PRIVATE
    EDS_MSG_BUF_MAX_STACK_BYTES=8192
)

# CAN FD (fd.conf): the FD code paths of ISO-TP, in every file that sees
# isotp_cfg_t, and an FD channel in src/main.c.
if(CONFIG_CAN_FD_MODE)
    target_compile_definitions(app PRIVATE ISOTP_ENABLE_CAN_FD=1)
endif()
