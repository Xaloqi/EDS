# examples/sensor_ecu/CMakeLists.txt
# =============================================================================
# Xaloqi EDS — SensorECU example
# Zephyr zone controller with real sensor integration.
# =============================================================================

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(sensor_ecu
    VERSION     1.0.0
    LANGUAGES   C
    DESCRIPTION "SensorECU — Xaloqi EDS sensor integration example"
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

set(DIAG_ROOT          "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(DIAG_CONFIG_YAML   "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics_config.yaml")
set(DIAG_CODEGEN_SCRIPT "${DIAG_ROOT}/tools/codegen.py")
set(DIAG_GENERATED_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/generated")
set(CODEGEN_SENTINEL    "${DIAG_GENERATED_DIR}/generated_config.h")

option(DIAG_SKIP_CODEGEN "Skip auto-running codegen (use existing generated/ files)" OFF)

# ---------------------------------------------------------------------------
# Code generation
# ---------------------------------------------------------------------------

if(NOT DIAG_SKIP_CODEGEN)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_FOUND)
        message(FATAL_ERROR "Python3 not found. Install it or use -DDIAG_SKIP_CODEGEN=ON")
    endif()

    file(GLOB DIAG_TEMPLATES "${DIAG_ROOT}/tools/templates/*.j2")

    add_custom_command(
        OUTPUT   "${CODEGEN_SENTINEL}"
        COMMAND  "${Python3_EXECUTABLE}"
                 "${DIAG_CODEGEN_SCRIPT}"
                 --config "${DIAG_CONFIG_YAML}"
                 --out    "${DIAG_GENERATED_DIR}"
                 --safety-wrappers --asil-level B
                 --no-manifest
        DEPENDS
            "${DIAG_CONFIG_YAML}"
            "${DIAG_CODEGEN_SCRIPT}"
            ${DIAG_TEMPLATES}
        COMMENT "[sensor_ecu] Generating sources from diagnostics_config.yaml..."
        VERBATIM
    )

    add_custom_target(run_codegen DEPENDS "${CODEGEN_SENTINEL}")
endif()

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------

target_include_directories(app PRIVATE
    ${DIAG_ROOT}/core
    ${DIAG_ROOT}/core/uds_services
    ${DIAG_ROOT}/transport
    ${DIAG_ROOT}/config
    ${DIAG_ROOT}/platform
    ${DIAG_ROOT}/platform/zephyr
    ${DIAG_GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src   # sensor_ecu.h
)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

# UDS service source list — set EDS_SERVICE_SRCS.
include(${DIAG_ROOT}/cmake/eds_service_sources.cmake)

target_sources(app PRIVATE

    # Application
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sensor_monitor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/did_handlers_impl.c

    # UDS core
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
    ${DIAG_ROOT}/core/uds_transfer_ctx.c

    # UDS service handlers (all 16 SIDs + dispatch table).
    ${EDS_SERVICE_SRCS}

    # ISO-TP + CAN
    ${DIAG_ROOT}/transport/isotp.c
    ${DIAG_ROOT}/transport/can_transport.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_can.c  # moved from transport/ — phase-housekeeping

    # Config databases
    ${DIAG_ROOT}/config/did_database.c
    ${DIAG_ROOT}/config/dtc_database.c
    ${DIAG_ROOT}/config/dtc_mirror.c
    ${DIAG_ROOT}/config/routine_database.c

    # Platform
    ${DIAG_ROOT}/platform/uds_flash_ops.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_port.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_mutex.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_timer.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_wdt.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_platform_api.c

    # Generated
    ${DIAG_GENERATED_DIR}/did_handlers.c
    ${DIAG_GENERATED_DIR}/uds_init.c
    ${DIAG_GENERATED_DIR}/did_safety_wrappers.c
    ${DIAG_GENERATED_DIR}/routine_handlers.c
)

# NVM store — mock on native_sim, real NVS on hardware
if(CONFIG_BOARD_NATIVE_SIM)
    target_sources(app PRIVATE ${DIAG_ROOT}/platform/zephyr/nvm_store_mock.c)
else()
    target_sources(app PRIVATE ${DIAG_ROOT}/platform/zephyr/nvm_store.c)
endif()

if(NOT DIAG_SKIP_CODEGEN AND TARGET run_codegen)
    add_dependencies(app run_codegen)
endif()

# Suppress the uds_msg_buf_t _Static_assert for all Zephyr example builds.
# The Zephyr task stacks for these examples (4096-6144 bytes) are large enough
# to hold the struct if needed, but all EDS stack buffers are module-level
# statics — the assert would fire as a false positive on every compilation unit.
# Zephyr's native_sim gets this via the CMakeLists.txt native_sim block;
# hardware targets need it here.
target_compile_definitions(app PRIVATE EDS_MSG_BUF_MAX_STACK_BYTES=8192)
