# File: examples/vehicle_state_ecu/CMakeLists.txt
# =============================================================================
# Xaloqi EDS
# examples/vehicle_state_ecu/CMakeLists.txt
#
# PURPOSE: Stand-alone Zephyr build file for the VehicleStateECU example.
#          native_sim only — see README.md.
#
#          cd examples/vehicle_state_ecu
#          west build -b native_sim .
#
# SPDX-License-Identifier: Apache-2.0
# =============================================================================

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(vehicle_state_ecu
    VERSION     0.1.0
    LANGUAGES   C
    DESCRIPTION "VehicleStateECU example — Xaloqi EDS"
)

set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

target_compile_options(app PRIVATE
    -Wall
    -Wextra
    -Wshadow
    -Wdouble-promotion
    -Wundef
    -Wno-unused-parameter
    -Wno-sign-conversion     # see examples/basic_ecu/CMakeLists.txt: Zephyr's
                             # cbprintf macros trip -Wsign-conversion on every
                             # LOG_* call.
    -fstack-usage
)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

set(DIAG_ROOT           "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(DIAG_CONFIG_YAML     "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics_config.yaml")
set(DIAG_CODEGEN_SCRIPT  "${DIAG_ROOT}/tools/codegen.py")
set(DIAG_GENERATED_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/generated")
set(CODEGEN_SENTINEL     "${DIAG_GENERATED_DIR}/safety_config.h")

# ---------------------------------------------------------------------------
# Codegen — OFF by default for this example (the one difference from
# basic_ecu's CMakeLists.txt pattern).
#
# generated/routine_handlers.c carries four hand-written [APP HOOK] bodies
# that delegate to src/vehicle_state.c — the actuation path's only
# attachment point (routine_database_find() returns `const`, so nothing
# can re-point a callback at runtime; see the banner at the top of that
# file and EDS#340). Re-running codegen would silently overwrite them with
# empty TODO stubs. This also means a forker without the commercial codegen
# tool (pyyaml/jinja2, tools/templates/) can still build — same OD-14
# Option 2 mechanism every other example's pre-committed generated/ relies
# on, just defaulted ON here instead of opted into with -DDIAG_SKIP_CODEGEN=ON.
#
# Regenerate deliberately (after editing diagnostics_config.yaml) with:
#   python3 tools/codegen.py --config examples/vehicle_state_ecu/diagnostics_config.yaml \
#                            --out    examples/vehicle_state_ecu/generated/ \
#                            --safety-wrappers --asil-level B --no-manifest
# then reapply the four [APP HOOK] bodies in generated/routine_handlers.c
# before committing.
# ---------------------------------------------------------------------------
option(DIAG_SKIP_CODEGEN
    "Skip auto-running tools/codegen.py (use pre-existing generated/ files)"
    ON
)

if(NOT DIAG_SKIP_CODEGEN)

    find_package(Python3 COMPONENTS Interpreter QUIET)

    if(NOT Python3_FOUND)
        message(FATAL_ERROR
            "\n"
            "[DIAG] ERROR: Python3 interpreter not found.\n"
            "       Cannot run tools/codegen.py to generate diagnostic sources.\n"
            "\n"
            "  Resolution options:\n"
            "    1. Install Python3 (>= 3.9) with required packages:\n"
            "         pip install pyyaml jinja2\n"
            "\n"
            "    2. Re-configure with -DDIAG_SKIP_CODEGEN=ON (the default for\n"
            "       this example) to use the pre-committed generated/ files.\n"
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
        COMMENT  "[DIAG] Generating sources from diagnostics_config.yaml (ASIL-B)... "
                 "NOTE: this will overwrite the hand-written [APP HOOK] bodies in "
                 "generated/routine_handlers.c -- reapply them before committing."
        VERBATIM
    )

    add_custom_target(run_codegen
        DEPENDS "${CODEGEN_SENTINEL}"
        COMMENT "[DIAG] run_codegen complete."
    )

else()

    message(STATUS
        "[DIAG] DIAG_SKIP_CODEGEN=ON (default for this example) — "
        "using pre-committed files in ${DIAG_GENERATED_DIR}"
    )

endif()

# ---------------------------------------------------------------------------
# [APP HOOK] guard — fail loudly, at configure time, if
# generated/routine_handlers.c was ever regenerated without reapplying the
# four hand-written delegations to src/vehicle_state.c (see above).
# ---------------------------------------------------------------------------
file(STRINGS "${DIAG_GENERATED_DIR}/routine_handlers.c" DIAG_APP_HOOK_LINES
     REGEX "\\[APP HOOK\\]")
list(LENGTH DIAG_APP_HOOK_LINES DIAG_APP_HOOK_COUNT)
if(DIAG_APP_HOOK_COUNT LESS 4)
    message(FATAL_ERROR
        "\n"
        "[DIAG] ERROR: generated/routine_handlers.c is missing its [APP HOOK]\n"
        "       markers (found ${DIAG_APP_HOOK_COUNT}, expected >= 4).\n"
        "       This file was regenerated and the hand-written delegations to\n"
        "       src/vehicle_state.c were lost. See the banner at the top of\n"
        "       generated/routine_handlers.c and reapply them before building.\n"
    )
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
include(${DIAG_ROOT}/cmake/eds_service_sources.cmake)

target_sources(app PRIVATE

    # ── Application ─────────────────────────────────────────────────────
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vehicle_state.c

    # ── UDS core ─────────────────────────────────────────────────────────
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

    ${EDS_SERVICE_SRCS}
    ${DIAG_ROOT}/core/uds_transfer_ctx.c
    ${DIAG_ROOT}/platform/uds_flash_ops.c
    ${DIAG_ROOT}/platform/uds_image_policy.c

    # ── ISO-TP and CAN transport ──────────────────────────────────────────
    ${DIAG_ROOT}/transport/isotp.c
    ${DIAG_ROOT}/transport/can_transport.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_can.c

    # ── Configuration databases ───────────────────────────────────────────
    ${DIAG_ROOT}/config/did_database.c
    ${DIAG_ROOT}/config/dtc_database.c
    ${DIAG_ROOT}/config/routine_database.c
    ${DIAG_ROOT}/config/dtc_mirror.c

    # ── Zephyr platform abstraction ───────────────────────────────────────
    ${DIAG_ROOT}/platform/zephyr/zephyr_port.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_mutex.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_timer.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_wdt.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_platform_api.c
    ${DIAG_ROOT}/platform/zephyr/nvm_store_mock.c   # native_sim only — see main.c

    # ── Generated sources ─────────────────────────────────────────────────
    ${DIAG_GENERATED_DIR}/did_handlers.c
    ${DIAG_GENERATED_DIR}/uds_init.c
    ${DIAG_GENERATED_DIR}/did_safety_wrappers.c
    ${DIAG_GENERATED_DIR}/routine_handlers.c
)

if(NOT DIAG_SKIP_CODEGEN AND TARGET run_codegen)
    add_dependencies(app run_codegen)
endif()

# Required for the build to compile: _Static_assert in uds_types.h fires at
# translation-unit level whenever sizeof(uds_msg_buf_t) > EDS_MSG_BUF_MAX_STACK_BYTES.
target_compile_definitions(app PRIVATE EDS_MSG_BUF_MAX_STACK_BYTES=8192)
