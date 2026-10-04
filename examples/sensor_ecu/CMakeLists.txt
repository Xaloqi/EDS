# examples/sensor_ecu/CMakeLists.txt
# =============================================================================
# Xaloqi EDS — SensorECU example
# Zephyr zone controller with real sensor integration.
#
# BUILDING:
#   # Simulation build (CI / host testing, no hardware required):
#   # [EDS#339] -DEXTRA_CONF_FILE/-DDTC_OVERLAY_FILE must be passed
#   # explicitly — this example's own boards/native_sim/native_sim.conf
#   # and .overlay (nested) are never auto-discovered by Zephyr (only a
#   # flat boards/<board>.conf is — same gap O-132 found and documented
#   # for examples/bms_ecu/CMakeLists.txt). Omitting them silently builds
#   # without CONFIG_SENSOR/CONFIG_REBOOT and without the sensor devicetree
#   # nodes at all, rather than failing the way EDS#339 describes.
#   west build -b native_sim examples/sensor_ecu \
#     -- -DDIAG_SKIP_CODEGEN=ON \
#     -DEXTRA_CONF_FILE=<eds>/examples/sensor_ecu/boards/native_sim/native_sim.conf \
#     -DDTC_OVERLAY_FILE=<eds>/examples/sensor_ecu/boards/native_sim/native_sim.overlay
# =============================================================================

cmake_minimum_required(VERSION 3.20.0)

# [EDS#339] Out-of-tree devicetree bindings for the native_sim-only stub
# sensor driver (drivers/sensor_stub/) — must be appended before
# find_package(Zephyr) parses devicetree.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

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

# [EDS#339] Defaulted ON for this example (the one difference from most
# other examples' CMakeLists.txt pattern, shared with vehicle_state_ecu).
# generated/did_handlers.c and generated/routine_handlers.c carry
# hand-written [APP HOOK] bodies that delegate to sensor_monitor.c's real
# state — did_entry_t.read_cb/write_cb and routine_entry_t's callbacks are
# bare function pointers copied by value into a fixed-capacity array at
# init (did_database_register()/routine_database_register()), so there is
# no way to re-point them at runtime; the callback bodies in the
# generated files are the only place this application's live behaviour
# can run. Re-running codegen would silently overwrite them with empty
# TODO stubs — the guard below fails loudly, at configure time, if that
# ever happens without the hooks being reapplied.
option(DIAG_SKIP_CODEGEN "Skip auto-running codegen (use existing generated/ files)" ON)

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
        COMMENT "[sensor_ecu] Generating sources from diagnostics_config.yaml... "
                "NOTE: this will overwrite the hand-written [APP HOOK] bodies in "
                "generated/did_handlers.c and generated/routine_handlers.c -- "
                "reapply them before committing."
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

    # [EDS#339] native_sim-only stub sensor driver backing the devicetree
    # nodes in boards/native_sim/native_sim.overlay (DTS_ROOT above).
    # Harmless to compile unconditionally: DEVICE_DT_INST_DEFINE expands
    # to nothing if no "xaloqi,ambient-temp-stub"/"xaloqi,voltage-stub"
    # node is status "okay" for the board being built.
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/sensor_stub/ambient_temp_stub.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/sensor_stub/voltage_stub.c

    # [#333] src/did_handlers_impl.c is deliberately NOT listed. It does not
    # exist in this example (only in sensor_ecu_freertos), and the generated
    # did_handlers.c below already defines did_handlers_register_all() and
    # s_did_table — listing both would be a duplicate-symbol link failure.
    # See sensor_ecu_freertos/CMakeLists.txt for the same hazard documented.

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
    ${DIAG_ROOT}/platform/uds_image_policy.c  # [#232] Image verification policy singleton
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

# ---------------------------------------------------------------------------
# [APP HOOK] guard — fail loudly, at configure time, if generated/
# did_handlers.c or generated/routine_handlers.c were ever regenerated
# without reapplying the hand-written delegations to sensor_monitor.c.
# ---------------------------------------------------------------------------
file(STRINGS "${DIAG_GENERATED_DIR}/did_handlers.c" DIAG_DID_APP_HOOK_LINES
     REGEX "\\[APP HOOK\\]")
list(LENGTH DIAG_DID_APP_HOOK_LINES DIAG_DID_APP_HOOK_COUNT)
if(DIAG_DID_APP_HOOK_COUNT LESS 5)
    message(FATAL_ERROR
        "\n"
        "[sensor_ecu] ERROR: generated/did_handlers.c is missing its\n"
        "       [APP HOOK] markers (found ${DIAG_DID_APP_HOOK_COUNT}, expected >= 5).\n"
        "       This file was regenerated and the hand-written delegations to\n"
        "       sensor_monitor.c were lost. See EDS#339 and reapply them\n"
        "       before building.\n"
    )
endif()

file(STRINGS "${DIAG_GENERATED_DIR}/routine_handlers.c" DIAG_RTN_APP_HOOK_LINES
     REGEX "\\[APP HOOK\\]")
list(LENGTH DIAG_RTN_APP_HOOK_LINES DIAG_RTN_APP_HOOK_COUNT)
if(DIAG_RTN_APP_HOOK_COUNT LESS 2)
    message(FATAL_ERROR
        "\n"
        "[sensor_ecu] ERROR: generated/routine_handlers.c is missing its\n"
        "       [APP HOOK] markers (found ${DIAG_RTN_APP_HOOK_COUNT}, expected >= 2).\n"
        "       This file was regenerated and the hand-written delegations to\n"
        "       sensor_monitor.c were lost. See EDS#339 and reapply them\n"
        "       before building.\n"
    )
endif()

# Suppress the uds_msg_buf_t _Static_assert for all Zephyr example builds.
# The Zephyr task stacks for these examples (4096-6144 bytes) are large enough
# to hold the struct if needed, but all EDS stack buffers are module-level
# statics — the assert would fire as a false positive on every compilation unit.
# Zephyr's native_sim gets this via the CMakeLists.txt native_sim block;
# hardware targets need it here.
target_compile_definitions(app PRIVATE EDS_MSG_BUF_MAX_STACK_BYTES=8192)
