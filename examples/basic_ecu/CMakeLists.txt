# File: examples/basic_ecu/CMakeLists.txt
# =============================================================================
# Xaloqi EDS
# examples/basic_ecu/CMakeLists.txt
#
# PURPOSE: Stand-alone Zephyr build file for the BasicECU example application.
#
#          Can be built either:
#            (a) From the project root CMakeLists.txt (sources pulled in there), OR
#            (b) Directly as a stand-alone Zephyr application:
#                  cd examples/basic_ecu
#                  cmake -B build -DBOARD=<board>
#                  cmake --build build
#
# PHASE 2B CHANGES:
#   - Code generation target renamed to 'run_codegen' (matches root CMake and spec).
#   - FATAL_ERROR on missing Python3 — no silent degradation.
#   - Template glob added as dependency — codegen re-runs if any .j2 changes.
#   - Include and source paths updated for Phase 2A generated files.
#   - Version bumped to 0.3.0.
#
# STAND-ALONE USAGE:
#   cd examples/basic_ecu
#   cmake -B build -DBOARD=native_posix      # simulation target
#   cmake --build build
#   ./build/zephyr/zephyr.exe               # run on host
#
#   # Skip codegen if generated/ files are already present:
#   cmake -B build -DBOARD=<board> -DDIAG_SKIP_CODEGEN=ON
#
#   # Force codegen only:
#   cmake --build build --target run_codegen
#
# SAFETY CONTEXT : Example application — not safety-assessed.
# CODING STANDARD: MISRA C:2012 alignment intended.
# VERSION        : 0.3.0 (Phase 2B)
# SPDX-License-Identifier: Apache-2.0
# =============================================================================

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(basic_ecu
    VERSION     0.4.0
    LANGUAGES   C
    DESCRIPTION "BasicECU example — Xaloqi EDS"
)

set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

# ---------------------------------------------------------------------------
# Compiler flags (mirrors root CMakeLists.txt for consistency)
# ---------------------------------------------------------------------------
target_compile_options(app PRIVATE
    # ── Enabled ───────────────────────────────────────────────────────────
    -Wall                    # standard: most likely bugs
    -Wextra                  # extra checks beyond -Wall
    -Wshadow                 # shadowed variable declarations
    -Wdouble-promotion       # implicit float-to-double (embedded risk)
    -Wundef                  # undefined macro used in #if
    -Wno-unused-parameter    # suppress noisy unused-param warnings

    # ── Intentionally removed ──────────────────────────────────────────────

    # -Wpedantic REMOVED.
    # Zephyr headers use three C11/GCC extensions that -Wpedantic rejects,
    # all confirmed in CI logs:
    #   _Static_assert (C11): BUILD_ASSERT macro in zephyr/toolchain/gcc.h:87.
    #     Every LOG_* call expands BUILD_ASSERT. CI: "ISO C99 does not support
    #     '_Static_assert' [-Wpedantic]"
    #   _Generic (C11): cbprintf_internal.h:630,658.
    #     CI: "ISO C99 does not support '_Generic' [-Wpedantic]"
    #   Zero-arg variadic macro: LOG_INF("string") with no format args.
    #     CI: zephyr_port.c:115 "ISO C99 requires at least one argument for
    #     the '...' in variadic macro"
    # Zephyr is not written to ISO C99 pedantic conformance.

    # -Wconversion REMOVED.
    # Zephyr's cbprintf macros emit -Wsign-conversion on every LOG_* call:
    #   _pkg_offset += sizeof(*_len_loc);  [cbprintf_internal.h:828]
    # CI log: "conversion to 'int' from 'unsigned int' [-Wsign-conversion]"
    # With 20+ source files × multiple log calls, hundreds of warnings fill
    # the GitHub Actions step log and trigger the truncation limit, hiding
    # the real error completely.
    -Wno-sign-conversion     # explicit: suppress cbprintf macro side-effect

    # ── Build analysis ─────────────────────────────────────────────────────
    -fstack-usage
)

# ---------------------------------------------------------------------------
# Paths (all relative to this file's directory)
# ---------------------------------------------------------------------------

# Project root (two levels up from examples/basic_ecu/)
set(DIAG_ROOT         "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# diagnostics_config.yaml — single source of truth.
set(DIAG_CONFIG_YAML  "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics_config.yaml")

# tools/codegen.py — code generator script.
set(DIAG_CODEGEN_SCRIPT "${DIAG_ROOT}/tools/codegen.py")

# Output directory for generated C files — lives inside this example directory.
# This matches the pattern used by all other examples (bms_ecu, ardep_ecu, etc.)
# and mirrors what customers do in their own projects:
#   python3 tools/codegen.py --config your_ecu/diagnostics_config.yaml \
#                            --out    your_ecu/generated/ ...
set(DIAG_GENERATED_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/generated")

# Sentinel: file whose timestamp CMake uses to detect when codegen must re-run.
# Use safety_config.h — only produced with --safety-wrappers, so it correctly
# triggers re-generation when the safety template changes.
set(CODEGEN_SENTINEL "${DIAG_GENERATED_DIR}/safety_config.h")

# ---------------------------------------------------------------------------
# Option: disable automatic code generation
# ---------------------------------------------------------------------------
option(DIAG_SKIP_CODEGEN
    "Skip auto-running tools/codegen.py (use pre-existing generated/ files)"
    OFF
)

# ---------------------------------------------------------------------------
# Custom target: run_codegen
#
# Generates under ${DIAG_GENERATED_DIR}/:
#   generated_config.h
#   did_handlers.h   did_handlers.c
#   uds_init.h       uds_init.c
#
# Re-runs when any of the following inputs are newer than the sentinel:
#   diagnostics_config.yaml
#   tools/codegen.py
#   tools/templates/*.j2
#
# BUILD-FAILURE POLICY:
#   If Python3 is absent and DIAG_SKIP_CODEGEN is OFF, configuration halts
#   with FATAL_ERROR. Silent generation failures can produce invisible build
#   errors that are extremely difficult to diagnose.
# ---------------------------------------------------------------------------
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
            "    2. Run codegen manually from the project root:\n"
            "         python3 tools/codegen.py \\\n"
            "             --config examples/basic_ecu/diagnostics_config.yaml \\\n"
            "             --out    examples/basic_ecu/generated/ \\\n"
            "             --safety-wrappers --asil-level B\n"
            "       Then re-configure with:\n"
            "         cmake -DDIAG_SKIP_CODEGEN=ON ..\n"
        )
    endif()

    # Collect Jinja2 templates as dependencies.
    file(GLOB DIAG_TEMPLATES
        "${DIAG_ROOT}/tools/templates/*.j2"
    )

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
        COMMENT  "[DIAG] Generating sources from diagnostics_config.yaml (ASIL-B)..."
        VERBATIM
    )

    # Named target — invoke explicitly with:
    #   cmake --build build --target run_codegen
    add_custom_target(run_codegen
        DEPENDS "${CODEGEN_SENTINEL}"
        COMMENT "[DIAG] run_codegen complete."
    )

else()

    message(STATUS
        "[DIAG] DIAG_SKIP_CODEGEN=ON — "
        "using existing files in ${DIAG_GENERATED_DIR}"
    )

endif() # NOT DIAG_SKIP_CODEGEN

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
    ${DIAG_GENERATED_DIR}          # generated_config.h, did_handlers.h, uds_init.h
)

# ---------------------------------------------------------------------------
# Source files
#
# Explicit absolute paths (via DIAG_ROOT) so this file can be used either as
# a stand-alone Zephyr app OR included from the root CMakeLists.txt without
# relative-path ambiguity.
# ---------------------------------------------------------------------------
# UDS service source list — set EDS_SERVICE_SRCS.
include(${DIAG_ROOT}/cmake/eds_service_sources.cmake)

target_sources(app PRIVATE

    # ── Application entry point ──────────────────────────────────────────
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c

    # ── UDS core ─────────────────────────────────────────────────────────
    ${DIAG_ROOT}/core/uds_server.c
    ${DIAG_ROOT}/core/uds_session.c
    ${DIAG_ROOT}/core/uds_security.c
    ${DIAG_ROOT}/core/uds_safety.c

    # ── UDS core: supporting modules ──────────────────────────────────────
    # FIX (B1): These files exist on disk but were absent from target_sources,
    # causing undefined-reference linker errors. Each is directly referenced
    # by a compiled translation unit in this build:
    #
    #   uds_security_algo.c  — seed/key derivation, called by service_0x27.c
    #   uds_aes_cmac.c       — AES-128-CMAC primitive, called by uds_security_algo.c
    #   uds_access_table.c   — ACL lookup/enforce, called by uds_server.c
    #   uds_comm_control.c   — comm-control state, called by service_0x28.c / 0x85.c
    #   uds_session_stats.c  — session counters, called by zephyr_port.c
    #   uds_security_nvm.c   — persistent security counters (NVM-backed)
    ${DIAG_ROOT}/core/uds_security_algo.c
    ${DIAG_ROOT}/core/uds_aes_cmac.c
    ${DIAG_ROOT}/core/uds_access_table.c
    ${DIAG_ROOT}/core/uds_comm_control.c
    ${DIAG_ROOT}/core/uds_session_stats.c
    ${DIAG_ROOT}/core/uds_security_nvm.c

    # UDS service handlers (all 16 SIDs + dispatch table).
    ${EDS_SERVICE_SRCS}
    ${DIAG_ROOT}/core/uds_transfer_ctx.c                # DFU transfer state machine + CRC-32
    ${DIAG_ROOT}/platform/uds_flash_ops.c               # Flash ops registration singleton

    # ── ISO-TP and CAN transport ──────────────────────────────────────────
    ${DIAG_ROOT}/transport/isotp.c
    ${DIAG_ROOT}/transport/can_transport.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_can.c  # moved from transport/ — phase-housekeeping

    # ── Configuration databases ───────────────────────────────────────────
    ${DIAG_ROOT}/config/did_database.c
    ${DIAG_ROOT}/config/dtc_database.c
    ${DIAG_ROOT}/config/routine_database.c

    # ── DTC mirror ────────────────────────────────────────────────────────
    # FIX (B1): dtc_mirror.c was absent.
    # zephyr_port.c calls dtc_mirror_flush_all(); service_0x14.c calls
    # dtc_mirror_clear_all(). Both produce "undefined reference" without this.
    ${DIAG_ROOT}/config/dtc_mirror.c

    # ── Zephyr platform abstraction ───────────────────────────────────────
    ${DIAG_ROOT}/platform/zephyr/zephyr_port.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_mutex.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_timer.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_wdt.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_platform_api.c  # eds_platform_* neutral interface

    # ── Generated sources ─────────────────────────────────────────────────
    ${DIAG_GENERATED_DIR}/did_handlers.c
    ${DIAG_GENERATED_DIR}/uds_init.c
    ${DIAG_GENERATED_DIR}/did_safety_wrappers.c
    ${DIAG_GENERATED_DIR}/routine_handlers.c
)

# ──────────────────────────────────────────────────────────────────────────────
# NVM store: platform-conditional source selection
#
# Two implementations of the nvm_store_* API exist:
#
#   platform/nvm_store.c      — real Zephyr NVS backend.
#     Includes <zephyr/fs/nvs.h> and <zephyr/drivers/flash.h>.
#     Requires CONFIG_FLASH=y, CONFIG_FLASH_MAP=y, CONFIG_NVS=y (set in
#     boards/nucleo_h743zi/nucleo_h743zi.conf) and a DTS flash partition
#     (defined in boards/nucleo_h743zi/nucleo_h743zi.overlay as "diag_nvs").
#     Used for all real hardware targets.
#
#   platform/nvm_store_mock.c — RAM-backed mock for host simulation.
#     Self-defines NVM_STORE_HOST_MOCK at the top of the file.
#     Provides the full nvm_store_* API without any Zephyr flash/NVS
#     subsystem dependency.
#     Used for native_sim, where NVS flash infrastructure is absent.
#
# Both files export the same symbols. Exactly one must be compiled per build.
# Including both would produce duplicate-symbol linker errors.
# ──────────────────────────────────────────────────────────────────────────────
if(CONFIG_BOARD_NATIVE_SIM)
    target_sources(app PRIVATE ${DIAG_ROOT}/platform/zephyr/nvm_store_mock.c)
else()
    target_sources(app PRIVATE ${DIAG_ROOT}/platform/zephyr/nvm_store.c)
endif()

# ---------------------------------------------------------------------------
if(NOT DIAG_SKIP_CODEGEN AND TARGET run_codegen)
    add_dependencies(app run_codegen)
endif()

# Required for the build to compile: _Static_assert in uds_types.h fires at
# translation-unit level whenever sizeof(uds_msg_buf_t) > EDS_MSG_BUF_MAX_STACK_BYTES.
# No uds_msg_buf_t is allocated on the task stack in this build — all instances
# are static members of their respective state structs (doip_server_state_t etc.).
target_compile_definitions(app PRIVATE EDS_MSG_BUF_MAX_STACK_BYTES=8192)
