# File: CMakeLists.txt
# =============================================================================
# Xaloqi EDS
# CMakeLists.txt  (project root)
#
# PURPOSE: Root build file — wires the full diagnostics stack into a single
#          Zephyr application target, with automatic code generation from
#          diagnostics_config.yaml before any source file is compiled.
#
# PHASE 2B ADDITIONS:
#   - Custom target 'run_codegen' drives tools/codegen.py.
#   - Build is aborted with FATAL_ERROR if Python3 is absent and generated
#     files do not exist (no silent degradation).
#   - Template glob dependency ensures codegen re-runs when any .j2 changes.
#   - Generated include path and source files wired into 'app' target.
#   - All layers ordered: application -> core -> transport -> config ->
#     platform -> generated.
#
# PHASE 3 ADDITIONS:
#   - core/uds_safety.c added to target_sources (safety check engine).
#   - generated/did_safety_wrappers.c added (ASIL-B per-DID safe accessors).
#
# FIX APPLIED (Technical Review — Issues B1, E1, E2):
#   B1: Added 10 source files that existed on disk but were absent from
#       target_sources, causing linker failures on their symbols:
#         core/uds_security_algo.c
#         core/uds_access_table.c
#         core/uds_comm_control.c
#         core/uds_session_stats.c
#         core/uds_security_nvm.c
#         core/uds_services/service_0x14.c
#         core/uds_services/service_0x19.c
#         core/uds_services/service_0x28.c
#         core/uds_services/service_0x85.c
#         config/dtc_mirror.c
#   E1: codegen invocation now passes --safety-wrappers so safety_config.h
#       is generated before uds_init.h includes it.
#   E2: config/dtc_mirror.c added (dtc_mirror_init() called by uds_init.c).
#
# FIX APPLIED (RC-NEW — nucleo_h743zi build failure, CI log 2026-03-10):
#   RC-NEW-1: -Wpedantic removed from target_compile_options.
#     Zephyr ARM headers (structs.h, exception.h, init.h, gcc.h, sflist.h,
#     __assert.h) use C11/GCC extensions (unnamed unions, named variadic macros,
#     zero-arg variadic macros) that are pedantic-errors under -std=c99.
#     GCC aborts compilation of transport/zephyr_can.c as a result.
#     Confirmed in CI log: "ninja: build stopped: subcommand failed" at
#     CMakeFiles/app.dir/.../transport/zephyr_can.c.obj (step [36/195]).
#   RC-NEW-2: -Wconversion removed from target_compile_options.
#     Zephyr cbprintf / log_msg macros emit -Wconversion on every LOG_* call
#     (util.h:577, log_msg.h:155). Cannot be suppressed per-callsite.
#   RC-NEW-3: -Wno-sign-conversion added to suppress cbprintf sign-conversion
#     noise (cbprintf_internal.h:828) that fills the build log.
#
# USAGE:
#   # Normal build (codegen runs automatically):
#   cmake -B build -DBOARD=<board>
#   cmake --build build
#
#   # Run codegen without compiling:
#   cmake --build build --target run_codegen
#
#   # Use pre-existing generated files (CI offline builds):
#   cmake -B build -DBOARD=<board> -DDIAG_SKIP_CODEGEN=ON
#   cmake --build build
#
# SAFETY CONTEXT : Contains ASIL-B candidate modules (ISO 26262 Part 6).
# CODING STANDARD: MISRA C:2012 alignment intended.
# VERSION        : 1.7.0
# SPDX-License-Identifier: Apache-2.0
# =============================================================================

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(zephyr_diagnostics_suite
    VERSION     1.7.0
    LANGUAGES   C
    DESCRIPTION "Production-grade UDS/ISO-TP diagnostics stack for Zephyr RTOS"
)

# ---------------------------------------------------------------------------
# C language standard
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)   # MISRA C:2012 Rule 1.2 — no language ext.

# ---------------------------------------------------------------------------
# Compiler diagnostic flags
#
# Consistent with MISRA C:2012 and ISO 26262-6 compiler configuration guidance.
# -fstack-usage emits per-function .su files used for stack budget analysis.
#
# FIX (RC-NEW — nucleo_h743zi build failure, confirmed CI log 2026-03-10):
#
#   -Wpedantic REMOVED.
#   Zephyr ARM headers use C11/GCC extensions that GCC rejects as pedantic-errors
#   when -Wpedantic is active under -std=c99 (which Zephyr enforces at the
#   translation-unit level regardless of CMAKE_C_STANDARD). Confirmed violations:
#     • zephyr/include/zephyr/arch/arm/structs.h:22
#         "struct has no members [-Wpedantic]"
#     • zephyr/include/zephyr/arch/arm/cortex_m/exception.h:103-109
#         "ISO C99 doesn't support unnamed structs/unions [-Wpedantic]"
#     • zephyr/include/zephyr/init.h:115
#         "ISO C99 doesn't support unnamed structs/unions [-Wpedantic]"
#     • zephyr/include/zephyr/toolchain/gcc.h:87
#         "ISO C does not permit named variadic macros [-Wvariadic-macros]"
#     • zephyr/include/zephyr/sys/__assert.h:57, sflist.h:302,319
#         "ISO C99 requires at least one argument for '...' in a variadic macro"
#   Under -std=c99 + -Wpedantic, GCC issues these as pedantic-errors (not
#   mere warnings), causing the compilation of transport/zephyr_can.c to abort.
#   Zephyr is not written to ISO C99 pedantic conformance. -Wpedantic must not
#   be applied to any translation unit that includes Zephyr headers.
#
#   -Wconversion REMOVED.
#   Zephyr's cbprintf / log_msg internal macros trigger -Wconversion on every
#   LOG_* call site in our code:
#     • zephyr/include/zephyr/sys/util.h:577
#         "conversion from 'int' to 'uint8_t' may change value [-Wconversion]"
#     • zephyr/include/zephyr/logging/log_msg.h:155
#         "conversion from 'unsigned int' to 'short unsigned int:11' [-Wconversion]"
#   These originate inside Zephyr macros; they cannot be suppressed without
#   disabling the flag entirely for the affected translation units.
#
#   -Wno-sign-conversion ADDED.
#   Explicit suppression of the sign-conversion subset (already present in
#   examples/basic_ecu/CMakeLists.txt). cbprintf_internal.h:828 emits this
#   on every LOG_* expansion. Without suppression it floods the build log
#   with hundreds of macro-expansion warnings, hiding real diagnostic output.
#
#   MISRA ALIGNMENT NOTE:
#   Removing -Wpedantic does NOT relax MISRA C:2012 compliance obligations.
#   MISRA violations are enforced by the static-analysis job (static_analysis_v2.py)
#   and a dedicated MISRA checker (future: pc-lint/polyspace). -Wpedantic is not
#   a substitute for a MISRA checker and should not be treated as one.
# ---------------------------------------------------------------------------
target_compile_options(app PRIVATE
    -Wall
    -Wextra
    -Wshadow
    -Wdouble-promotion
    -Wundef
    -Wno-unused-parameter
    -Wno-sign-conversion
    -fstack-usage

    # -Wpedantic intentionally absent — see FIX note above.
    # -Wconversion intentionally absent — see FIX note above.
)

# ---------------------------------------------------------------------------
# Configurable paths
# ---------------------------------------------------------------------------

# Single source of truth for all diagnostic parameters.
set(DIAG_CONFIG_YAML
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/basic_ecu/diagnostics_config.yaml"
    CACHE FILEPATH "Path to diagnostics_config.yaml"
)

# Output directory for files generated by tools/codegen.py.
# Placed inside the example ECU tree so its CMakeLists can resolve headers
# with a relative path, and so the Zephyr build system finds them.
set(DIAG_GENERATED_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/basic_ecu/generated"
    CACHE PATH "Output directory for generated C files"
)

# Path to the code generator script.
set(DIAG_CODEGEN_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen.py"
    CACHE FILEPATH "Path to tools/codegen.py"
)

# ---------------------------------------------------------------------------
# Option: disable automatic code generation
# ---------------------------------------------------------------------------
option(DIAG_SKIP_CODEGEN
    "Skip automatic code generation (use pre-existing files in DIAG_GENERATED_DIR)"
    OFF
)

# ---------------------------------------------------------------------------
# Custom target: run_codegen
#
# Invokes tools/codegen.py to produce output files under DIAG_GENERATED_DIR.
#
# FIX (Issue E1): --safety-wrappers added so safety_config.h is generated
# alongside the standard outputs. uds_init.h #includes safety_config.h, so
# it must exist before any translation unit that includes uds_init.h is
# compiled. Without --safety-wrappers the Zephyr build fails immediately
# with a "safety_config.h: No such file or directory" error.
#
# Inputs that trigger a rebuild when changed:
#   diagnostics_config.yaml
#   tools/codegen.py
#   tools/templates/*.j2  (via DIAG_TEMPLATES glob)
# ---------------------------------------------------------------------------
if(NOT DIAG_SKIP_CODEGEN)

    find_package(Python3 COMPONENTS Interpreter QUIET)

    if(NOT Python3_FOUND)
        message(FATAL_ERROR
            "\n"
            "[DIAG] ERROR: Python3 interpreter not found.\n"
            "       Code generation from diagnostics_config.yaml cannot proceed.\n"
            "\n"
            "  Resolution options:\n"
            "    1. Install Python3 (>= 3.8) and add it to PATH.\n"
            "       Then install required packages:\n"
            "         pip install pyyaml jinja2\n"
            "\n"
            "    2. Run codegen manually, then re-configure with:\n"
            "         -DDIAG_SKIP_CODEGEN=ON\n"
            "       Manual command:\n"
            "         python3 tools/codegen.py \\\n"
            "             --config examples/basic_ecu/diagnostics_config.yaml \\\n"
            "             --out    examples/basic_ecu/generated/ \\\n"
            "             --safety-wrappers --asil-level B\n"
            "\n"
            "    3. Commit pre-generated files to source control and use:\n"
            "         cmake -DDIAG_SKIP_CODEGEN=ON ..\n"
        )
    endif()

    # Collect all Jinja2 templates as dependencies so codegen re-runs
    # whenever any template is modified, not just when the YAML changes.
    file(GLOB DIAG_TEMPLATES
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/templates/*.j2"
    )

    # Sentinel: the file whose timestamp CMake uses to decide on re-generation.
    # Use safety_config.h as sentinel because it is only produced with
    # --safety-wrappers; using generated_config.h would not re-trigger
    # generation when only the safety template changes.
    set(CODEGEN_SENTINEL "${DIAG_GENERATED_DIR}/safety_config.h")

    add_custom_command(
        OUTPUT   "${CODEGEN_SENTINEL}"
        COMMAND  "${Python3_EXECUTABLE}"
                 "${DIAG_CODEGEN_SCRIPT}"
                 --config          "${DIAG_CONFIG_YAML}"
                 --out             "${DIAG_GENERATED_DIR}"
                 --safety-wrappers
                 --asil-level      B
                 --no-manifest
        DEPENDS
            "${DIAG_CONFIG_YAML}"
            "${DIAG_CODEGEN_SCRIPT}"
            ${DIAG_TEMPLATES}
        COMMENT  "[DIAG] Generating C sources from diagnostics_config.yaml (ASIL-B)..."
        VERBATIM
    )

    # Named target so users can invoke codegen explicitly:
    #   cmake --build build --target run_codegen
    add_custom_target(run_codegen
        DEPENDS "${CODEGEN_SENTINEL}"
        COMMENT "[DIAG] run_codegen: generation complete."
    )

else()

    message(STATUS
        "[DIAG] DIAG_SKIP_CODEGEN=ON — "
        "using existing files in ${DIAG_GENERATED_DIR}"
    )

endif() # NOT DIAG_SKIP_CODEGEN

# ---------------------------------------------------------------------------
# Include directories
#
# All paths are PRIVATE per the Zephyr convention (the 'app' target is the
# final executable — no other target links against it).
#
# Order: core -> services -> transport -> config -> platform -> generated
# (generated last: it #includes core headers, not the reverse)
# ---------------------------------------------------------------------------
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/uds_services
    ${CMAKE_CURRENT_SOURCE_DIR}/transport
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${DIAG_GENERATED_DIR}
)

# Phase 1 security: version annotation
set(DIAG_PHASE "6A")

# ---------------------------------------------------------------------------
# Source files
#
# Layer order mirrors the include-directory order above.
#
# FIX (Issue B1): Added the following files that were present on disk but
# missing from target_sources, causing undefined symbol linker errors:
#
#   core/uds_security_algo.c   — seed generation, key derivation, CT compare
#   core/uds_access_table.c    — session/security access matrix
#   core/uds_comm_control.c    — 0x28/0x85 communication control state
#   core/uds_session_stats.c   — session event counters (NVM-backed)
#   core/uds_security_nvm.c    — security attempt counter persistence
#   service_0x14.c             — ClearDiagnosticInformation
#   service_0x19.c             — ReadDTCInformation
#   service_0x28.c             — CommunicationControl
#   service_0x85.c             — ControlDTCSetting
#   config/dtc_mirror.c        — DTC status persistence to NVM
#
# FIX (Issue E2): config/dtc_mirror.c added — dtc_mirror_init() is called
# by the generated uds_init.c; the linker previously failed on this symbol.
#
# PHASE 3 NOTE: core/uds_safety.c and generated/did_safety_wrappers.c are
# included. uds_safety.c provides the ASIL-B check engine initialised
# by uds_generated_init(). did_safety_wrappers.c provides per-DID safe
# accessor functions that enforce session, security, and bounds checks.
# ---------------------------------------------------------------------------
target_sources(app PRIVATE

    # ── Application entry point ──────────────────────────────────────────
    examples/basic_ecu/src/main.c

    # ── UDS core ─────────────────────────────────────────────────────────
    core/uds_server.c
    core/uds_session.c
    core/uds_session_stats.c          # ADDED — session event counters
    core/uds_security.c
    core/uds_aes_cmac.c               # ADDED [P1-SEC] — AES-128-CMAC primitive
    core/uds_security_algo.c          # UPDATED [P1-SEC] — AES-CMAC + TRNG algo
    core/uds_security_nvm.c           # ADDED — security attempt persistence
    core/uds_safety.c
    core/uds_access_table.c           # ADDED — session/security access matrix
    core/uds_comm_control.c           # ADDED — comm control state (0x28/0x85)

    # ── UDS service handlers + dispatch table ─────────────────────────────
    core/uds_services/service_registration.c
    core/uds_services/service_0x10.c
    core/uds_services/service_0x11.c
    core/uds_services/service_0x14.c  # ADDED — ClearDiagnosticInformation
    core/uds_services/service_0x19.c  # ADDED — ReadDTCInformation
    core/uds_services/service_0x22.c
    core/uds_services/service_0x27.c
    core/uds_services/service_0x28.c  # ADDED — CommunicationControl
    core/uds_services/service_0x2E.c
    core/uds_services/service_0x2F.c  # ADDED — InputOutputControlByIdentifier
    core/uds_services/service_0x31.c  # ADDED — RoutineControl
    core/uds_services/service_0x34.c  # ADDED — RequestDownload (DFU)
    core/uds_services/service_0x36.c  # ADDED — TransferData (DFU)
    core/uds_services/service_0x37.c  # ADDED — RequestTransferExit (DFU)
    core/uds_services/service_0x3E.c
    core/uds_services/service_0x85.c  # ADDED — ControlDTCSetting
    core/uds_transfer_ctx.c           # ADDED — Transfer state machine + CRC-32

    # ── ISO-TP and CAN transport ──────────────────────────────────────────
    # transport/isotp.c and can_transport.c are RTOS-agnostic.
    # zephyr_can.c is Zephyr-specific and compiled in the EDS_PLATFORM=zephyr
    # block below alongside the other Zephyr HAL sources.
    transport/isotp.c
    transport/can_transport.c

    # ── Configuration databases ───────────────────────────────────────────
    config/did_database.c
    config/dtc_database.c
    config/dtc_mirror.c               # ADDED — DTC status NVM persistence
    config/routine_database.c         # ADDED — RoutineControl RID registry

    # ── Generated sources (do NOT edit — regenerate via run_codegen) ──────
    ${DIAG_GENERATED_DIR}/did_handlers.c
    ${DIAG_GENERATED_DIR}/uds_init.c
    ${DIAG_GENERATED_DIR}/did_safety_wrappers.c
    ${DIAG_GENERATED_DIR}/routine_handlers.c
)

# ---------------------------------------------------------------------------
# Platform abstraction: EDS_PLATFORM-conditional source selection
#
# EDS_PLATFORM selects the target RTOS platform:
#   "zephyr"   (default) — Zephyr RTOS, configured via DTS + Kconfig
#   "freertos"           — FreeRTOS, customer provides CAN + NVM callbacks
#
# Usage:
#   Zephyr (west build):   EDS_PLATFORM defaults to "zephyr"; no action needed.
#   FreeRTOS (cmake):      cmake -DEDS_PLATFORM=freertos -DFREERTOS_DIR=... .
#
# Each platform provides:
#   - eds_platform_* functions (reset, NVM flush, uptime, init, CAN input)
#   - CAN transport shim (implements can_transport_ops_t)
#   - NVM store implementation (implements nvm_store_* API)
#   - Flash ops (Zephyr only — MCUboot secondary slot; FreeRTOS: customer provides)
# ---------------------------------------------------------------------------

if(NOT DEFINED EDS_PLATFORM)
    set(EDS_PLATFORM "zephyr")
endif()

if(EDS_PLATFORM STREQUAL "zephyr")

    target_sources(app PRIVATE
        platform/zephyr/zephyr_can.c        # moved from transport/ — Zephyr CAN driver adapter
        platform/zephyr/zephyr_port.c
        platform/zephyr/zephyr_mutex.c
        platform/zephyr/zephyr_timer.c
        platform/zephyr/zephyr_wdt.c
        platform/zephyr/zephyr_platform_api.c
        platform/uds_flash_ops.c
    )

    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/zephyr
    )

    # NVM store: real Zephyr NVS on hardware, RAM mock on native_sim.
    if(CONFIG_BOARD_NATIVE_SIM)
        target_sources(app PRIVATE platform/zephyr/nvm_store_mock.c)
        # native_sim runs on the host OS with a large process stack.
        # Suppress the uds_msg_buf_t _Static_assert that guards against
        # accidental stack allocation on embedded targets.
        target_compile_definitions(app PRIVATE EDS_MSG_BUF_MAX_STACK_BYTES=8192)
    else()
        target_sources(app PRIVATE platform/zephyr/nvm_store.c)
        target_sources(app PRIVATE platform/zephyr/zephyr_flash_ops.c)
    endif()

elseif(EDS_PLATFORM STREQUAL "freertos")

    if(NOT DEFINED FREERTOS_DIR)
        message(FATAL_ERROR
            "EDS_PLATFORM=freertos requires -DFREERTOS_DIR=<path/to/FreeRTOS-Kernel>")
    endif()

    target_compile_definitions(app PRIVATE EDS_PLATFORM_FREERTOS=1)

    target_sources(app PRIVATE
        platform/freertos/freertos_platform_api.c
        platform/freertos/freertos_can.c
        platform/freertos/freertos_nvm.c
        platform/uds_flash_ops.c
        # FreeRTOS kernel sources
        ${FREERTOS_DIR}/tasks.c
        ${FREERTOS_DIR}/queue.c
        ${FREERTOS_DIR}/list.c
        ${FREERTOS_DIR}/timers.c
        ${FREERTOS_DIR}/portable/GCC/ARM_CM4F/port.c
    )

    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/freertos
        ${FREERTOS_DIR}/include
        ${FREERTOS_DIR}/portable/GCC/ARM_CM4F
        # Customer must provide FreeRTOSConfig.h — add its directory here:
        # ${CMAKE_CURRENT_SOURCE_DIR}/your_freertos_config_dir
    )

    # FreeRTOS NVM: customer provides flash ops via eds_platform_init().
    # freertos_nvm.c provides the nvm_store_* API using those ops.
    # No additional NVM source needed here.

else()
    message(FATAL_ERROR
        "Unknown EDS_PLATFORM '${EDS_PLATFORM}'. Valid values: 'zephyr', 'freertos'.")
endif()

# ---------------------------------------------------------------------------
# Dependency: app must not compile before run_codegen completes.
#
# This ensures that generated_config.h, did_handlers.h, uds_init.h,
# did_safety_wrappers.h, and safety_config.h are present on disk before
# any #include in the source list is resolved.
# ---------------------------------------------------------------------------
if(NOT DIAG_SKIP_CODEGEN AND TARGET run_codegen)
    add_dependencies(app run_codegen)
endif()

# ✅ End of CMakeLists.txt
