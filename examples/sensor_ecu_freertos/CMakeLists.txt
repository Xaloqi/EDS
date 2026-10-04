# =============================================================================
# Xaloqi EDS
# examples/sensor_ecu_freertos/CMakeLists.txt
#
# PURPOSE: Standalone CMake build for the FreeRTOS SensorECU example.
#
#          Demonstrates the complete Xaloqi EDS sensor → DID → DTC pattern
#          on FreeRTOS. Uses the same diagnostics_config.yaml as
#          examples/sensor_ecu/ — only the platform layer differs.
#
# USAGE:
#   # 1. Run codegen first (platform-independent):
#   python3 ../../tools/codegen.py \
#       --config diagnostics_config.yaml \
#       --out    generated/ \
#       --safety-wrappers --asil-level B --no-manifest
#
#   # 2. Build for QEMU Cortex-M4 (CI):
#   cmake -B build \
#       -DEDS_PLATFORM=freertos \
#       -DFREERTOS_DIR=/path/to/FreeRTOS-Kernel \
#       -DBOARD=qemu_cortex_m4 \
#       -GNinja .
#   ninja -C build
#
#   # 3. Build for STM32F4 / STM32H7:
#   cmake -B build \
#       -DEDS_PLATFORM=freertos \
#       -DFREERTOS_DIR=/path/to/FreeRTOS-Kernel \
#       -DBOARD=stm32f4 \
#       -GNinja .
#   ninja -C build
#
# CI JOB: freertos-examples (matrix, .github/workflows/ci.yml, EDS#329)
#   Builds this example with QEMU ARM Cortex-M4 and verifies the ELF is
#   produced.
# =============================================================================

cmake_minimum_required(VERSION 3.20)

# Cross-compilation toolchain for ARM Cortex-M4
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_SYSTEM_NAME Generic)
    set(CMAKE_SYSTEM_PROCESSOR arm)
    find_program(ARM_GCC arm-none-eabi-gcc)
    if(ARM_GCC)
        set(CMAKE_C_COMPILER arm-none-eabi-gcc)
        set(CMAKE_OBJCOPY arm-none-eabi-objcopy)
        set(CMAKE_SIZE    arm-none-eabi-size)
    else()
        find_program(ZEPHYR_GCC arm-zephyr-eabi-gcc)
        if(ZEPHYR_GCC)
            set(CMAKE_C_COMPILER arm-zephyr-eabi-gcc)
        endif()
    endif()
endif()

project(xaloqi_eds_sensor_ecu_freertos C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Required parameters
# ---------------------------------------------------------------------------


if(NOT DEFINED EDS_PLATFORM)
    set(EDS_PLATFORM "freertos")
endif()

if(NOT EDS_PLATFORM STREQUAL "freertos")
    message(FATAL_ERROR "This CMakeLists is for FreeRTOS only. Set -DEDS_PLATFORM=freertos")
endif()

if(NOT DEFINED FREERTOS_DIR)
    message(FATAL_ERROR
        "FREERTOS_DIR must point to the FreeRTOS-Kernel directory.\n"
        "Example: -DFREERTOS_DIR=/opt/freertos-kernel")
endif()

# [EDS#331] FREERTOS_DIR must be absolute before any if(EXISTS) test below.
# if(EXISTS) is only well-defined for absolute paths, so a relative
# FREERTOS_DIR silently satisfies the FreeRTOS-port check further down and
# then fails much later at add_executable() with "Cannot find source file:
# <rel>/tasks.c" plus a misleading "No SOURCES given to target". Reject it
# here with the actual cause rather than guessing a base directory - a guess
# could silently resolve to the wrong tree, which is worse than an error.
if(NOT IS_ABSOLUTE "${FREERTOS_DIR}")
    message(FATAL_ERROR
        "FREERTOS_DIR must be an absolute path (got: ${FREERTOS_DIR}).\n"
        "  -DFREERTOS_DIR=/opt/freertos-kernel\n"
        "  -DFREERTOS_DIR=\"$ENV{PWD}/../FreeRTOS-Kernel\" (shell-expanded)")
endif()
if(NOT IS_DIRECTORY "${FREERTOS_DIR}")
    message(FATAL_ERROR "FREERTOS_DIR does not exist: ${FREERTOS_DIR}")
endif()

if(NOT DEFINED BOARD)
    set(BOARD "qemu_cortex_m4")
endif()

# ---------------------------------------------------------------------------
# Repo root (two levels up)
# ---------------------------------------------------------------------------

set(DIAG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# [SEC-BUILD-MODE-01 / issue #84] shared FreeRTOS build-mode option -- see
# cmake/eds_build_mode.cmake for why this is centralized rather than
# declared per-example (same lesson as cmake/eds_service_sources.cmake).
include(${DIAG_ROOT}/cmake/eds_build_mode.cmake)

# ---------------------------------------------------------------------------
# Board-specific compiler flags
# ---------------------------------------------------------------------------

# Shared example board support (vector table, FreeRTOS hooks, boot log).
# See examples/common/boards/qemu_cortex_m4/startup.c — without a vector table
# ENTRY(Reset_Handler) has no root and --gc-sections deletes the whole image
# (EDS issue #268).
set(EDS_COMMON_BOARDS "${DIAG_ROOT}/examples/common/boards")

if(BOARD STREQUAL "qemu_cortex_m4")
    set(CPU_FLAGS -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
    set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/boards/qemu_cortex_m4/linker.ld")
    set(BOARD_SRCS
        ${EDS_COMMON_BOARDS}/qemu_cortex_m4/startup.c
        ${EDS_COMMON_BOARDS}/qemu_cortex_m4/freertos_hooks.c
        ${EDS_COMMON_BOARDS}/qemu_cortex_m4/board_log.c
    )
elseif(BOARD STREQUAL "stm32h7" OR BOARD STREQUAL "stm32f4")
    # EDS#272: these used to "build" — stm32h7 pointed at a linker script that
    # doesn't exist (silently falling through to no memory layout at all), and
    # stm32f4 shared qemu_cortex_m4's script, whose FLASH origin (0x00000000)
    # is wrong for a real STM32F4 (0x08000000). Neither ever produced a
    # bootable image; nothing caught it because nothing ran these images
    # before #268. Fail loudly instead of shipping an option that doesn't work.
    message(FATAL_ERROR
        "BOARD '${BOARD}' has no real board support in this example — it "
        "previously built an unbootable image silently (missing vector "
        "table, or the wrong flash origin) instead of failing (EDS#272). "
        "Add boards/${BOARD}/{linker.ld,startup.c} with the correct memory "
        "map and IRQ vector table for the real part first — see "
        "examples/safeboot_freertos_ecu/boards/nucleo_h743zi/ for the "
        "pattern this example doesn't yet have.")
else()
    message(FATAL_ERROR
        "Unknown BOARD '${BOARD}'. Supported: qemu_cortex_m4. Add real "
        "board support (linker script + startup.c, see "
        "examples/safeboot_freertos_ecu/boards/nucleo_h743zi/ for the "
        "pattern) before adding a new BOARD value here — see EDS#268/#272; "
        "a build that silently falls back to generic Cortex-M4 flags with "
        "no linker script is exactly the defect those fixed.")
endif()

# ---------------------------------------------------------------------------
# FreeRTOSConfig.h location
# ---------------------------------------------------------------------------

if(NOT DEFINED FREERTOS_CONFIG_DIR)
    set(FREERTOS_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/qemu_cortex_m4")
endif()

# [EDS#331] Same absolute-path requirement as FREERTOS_DIR. The default set
# above derives from CMAKE_CURRENT_SOURCE_DIR and is always absolute; this
# only constrains a user-supplied value.
if(NOT IS_ABSOLUTE "${FREERTOS_CONFIG_DIR}")
    message(FATAL_ERROR
        "FREERTOS_CONFIG_DIR must be an absolute path (got: ${FREERTOS_CONFIG_DIR}).")
endif()
if(NOT EXISTS "${FREERTOS_CONFIG_DIR}/FreeRTOSConfig.h")
    message(FATAL_ERROR
        "No FreeRTOSConfig.h in FREERTOS_CONFIG_DIR: ${FREERTOS_CONFIG_DIR}")
endif()

# ---------------------------------------------------------------------------
# FreeRTOS port selection
# ---------------------------------------------------------------------------

set(FREERTOS_PORT_DIR "${FREERTOS_DIR}/portable/GCC/ARM_CM4F")
if(NOT EXISTS "${FREERTOS_PORT_DIR}/port.c")
    set(FREERTOS_PORT_DIR "${FREERTOS_DIR}/portable/GCC/ARM_CM3")
    if(NOT EXISTS "${FREERTOS_PORT_DIR}/port.c")
        message(FATAL_ERROR "Cannot find FreeRTOS Cortex-M4/M3 port in ${FREERTOS_DIR}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Generated sources — run codegen before cmake
# ---------------------------------------------------------------------------

set(GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated")

if(NOT EXISTS "${GENERATED_DIR}/uds_init.c")
    message(STATUS
        "NOTE: generated/ is empty. Run codegen before building:\n"
        "  python3 ${DIAG_ROOT}/tools/codegen.py \\\n"
        "    --config ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics_config.yaml \\\n"
        "    --out ${GENERATED_DIR}/ \\\n"
        "    --safety-wrappers --asil-level B --no-manifest\n"
        "  then reapply the [APP HOOK] bodies in generated/did_handlers.c "
        "and generated/routine_handlers.c (EDS#349) before building.")
endif()

# ---------------------------------------------------------------------------
# [APP HOOK] guard — fail loudly, at configure time, if generated/
# did_handlers.c or generated/routine_handlers.c were ever regenerated
# without reapplying the hand-written delegations to
# sensor_monitor_freertos.c. Codegen here is a manual step (no CMake
# custom_command, unlike examples/sensor_ecu and examples/vehicle_state_ecu)
# so this is the only automatic check that would catch a forgotten reapply.
# ---------------------------------------------------------------------------
if(EXISTS "${GENERATED_DIR}/did_handlers.c")
    file(STRINGS "${GENERATED_DIR}/did_handlers.c" DIAG_DID_APP_HOOK_LINES
         REGEX "\\[APP HOOK\\]")
    list(LENGTH DIAG_DID_APP_HOOK_LINES DIAG_DID_APP_HOOK_COUNT)
    if(DIAG_DID_APP_HOOK_COUNT LESS 5)
        message(FATAL_ERROR
            "\n"
            "[sensor_ecu_freertos] ERROR: generated/did_handlers.c is missing\n"
            "       its [APP HOOK] markers (found ${DIAG_DID_APP_HOOK_COUNT}, expected >= 5).\n"
            "       This file was regenerated and the hand-written delegations to\n"
            "       sensor_monitor_freertos.c were lost. See EDS#349 and reapply\n"
            "       them before building.\n"
        )
    endif()
endif()

if(EXISTS "${GENERATED_DIR}/routine_handlers.c")
    file(STRINGS "${GENERATED_DIR}/routine_handlers.c" DIAG_RTN_APP_HOOK_LINES
         REGEX "\\[APP HOOK\\]")
    list(LENGTH DIAG_RTN_APP_HOOK_LINES DIAG_RTN_APP_HOOK_COUNT)
    if(DIAG_RTN_APP_HOOK_COUNT LESS 2)
        message(FATAL_ERROR
            "\n"
            "[sensor_ecu_freertos] ERROR: generated/routine_handlers.c is missing\n"
            "       its [APP HOOK] markers (found ${DIAG_RTN_APP_HOOK_COUNT}, expected >= 2).\n"
            "       This file was regenerated and the hand-written delegations to\n"
            "       sensor_monitor_freertos.c were lost. See EDS#349 and reapply\n"
            "       them before building.\n"
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------

# UDS service source list — set EDS_SERVICE_SRCS.
include(${DIAG_ROOT}/cmake/eds_service_sources.cmake)

add_executable(eds_sensor_freertos
    # Board support: reset vector, C runtime startup, FreeRTOS static-
    # allocation hooks, boot log. Empty for boards that supply their own.
    ${BOARD_SRCS}

    # Application
    src/main.c
    src/sensor_monitor_freertos.c
    # [EDS#349] src/did_handlers_impl.c no longer exists — it was dead code,
    # wired to a CI step ("Install sensor DID implementations") that never
    # existed. Its logic now lives directly in generated/did_handlers.c's
    # [APP HOOK] bodies (see that file's header comment).

    # EDS core
    ${DIAG_ROOT}/core/uds_server.c
    ${DIAG_ROOT}/core/uds_session.c
    ${DIAG_ROOT}/core/uds_session_stats.c
    ${DIAG_ROOT}/core/uds_security.c
    ${DIAG_ROOT}/core/uds_aes_cmac.c
    ${DIAG_ROOT}/core/uds_security_algo.c
    ${DIAG_ROOT}/core/uds_security_nvm.c
    ${DIAG_ROOT}/core/uds_safety.c
    ${DIAG_ROOT}/core/uds_access_table.c
    ${DIAG_ROOT}/core/uds_comm_control.c
    ${DIAG_ROOT}/core/uds_transfer_ctx.c

    # UDS service handlers (all 16 SIDs + dispatch table).
    ${EDS_SERVICE_SRCS}

    # Transport
    ${DIAG_ROOT}/transport/isotp.c
    ${DIAG_ROOT}/transport/can_transport.c

    # Config
    ${DIAG_ROOT}/config/did_database.c
    ${DIAG_ROOT}/config/dtc_database.c
    ${DIAG_ROOT}/config/dtc_mirror.c
    ${DIAG_ROOT}/config/routine_database.c

    # FreeRTOS platform HAL
    ${DIAG_ROOT}/platform/freertos/freertos_platform_api.c
    ${DIAG_ROOT}/platform/freertos/freertos_can.c
    ${DIAG_ROOT}/platform/freertos/freertos_nvm.c
    ${DIAG_ROOT}/platform/uds_flash_ops.c
    ${DIAG_ROOT}/platform/uds_image_policy.c  # [#232] Image verification policy singleton

    # Generated sources
    # did_handlers.c's [APP HOOK] bodies contain the real sensor
    # implementations directly (EDS#349) — no copy step involved.
    ${GENERATED_DIR}/uds_init.c
    ${GENERATED_DIR}/did_handlers.c
    ${GENERATED_DIR}/did_safety_wrappers.c
    ${GENERATED_DIR}/routine_handlers.c

    # FreeRTOS kernel
    ${FREERTOS_DIR}/tasks.c
    ${FREERTOS_DIR}/queue.c
    ${FREERTOS_DIR}/list.c
    ${FREERTOS_DIR}/timers.c
    ${FREERTOS_PORT_DIR}/port.c
    ${FREERTOS_DIR}/portable/MemMang/heap_4.c
)

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------

target_include_directories(eds_sensor_freertos PRIVATE
    ${DIAG_ROOT}/core
    ${DIAG_ROOT}/core/uds_services
    ${DIAG_ROOT}/transport
    ${DIAG_ROOT}/config
    ${DIAG_ROOT}/platform
    ${DIAG_ROOT}/platform/freertos
    ${EDS_COMMON_BOARDS}
    ${GENERATED_DIR}
    src
    # FreeRTOS
    ${FREERTOS_DIR}/include
    ${FREERTOS_PORT_DIR}
    ${FREERTOS_CONFIG_DIR}
)

# ---------------------------------------------------------------------------
# Compile definitions
# ---------------------------------------------------------------------------

target_compile_definitions(eds_sensor_freertos PRIVATE
    EDS_PLATFORM_FREERTOS=1
    # [SEC-BUILD-MODE-01 / issue #84] Give EDS_BUILD_IS_PRODUCTION the same
    # defined-and-1-or-0 signal Zephyr's Kconfig already gives it. The
    # $<BOOL:...> generator expression emits 1 when EDS_PLACEHOLDER_KEYS_ONLY
    # is ON, 0 when OFF -- production is declared with
    # -DEDS_PLACEHOLDER_KEYS_ONLY=OFF (see cmake/eds_build_mode.cmake).
    CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=$<BOOL:${EDS_PLACEHOLDER_KEYS_ONLY}>
    # Suppress the uds_msg_buf_t _Static_assert: the FreeRTOS example
    # never allocates uds_msg_buf_t on the stack.
    EDS_MSG_BUF_MAX_STACK_BYTES=8192
)

# ---------------------------------------------------------------------------
# Compile options
# ---------------------------------------------------------------------------

target_compile_options(eds_sensor_freertos PRIVATE
    ${CPU_FLAGS}
    -std=c11
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Os
    -ffunction-sections
    -fdata-sections
    -fno-exceptions
)

# ---------------------------------------------------------------------------
# Linker options
# ---------------------------------------------------------------------------

if(EXISTS "${LINKER_SCRIPT}")
    target_link_options(eds_sensor_freertos PRIVATE
        ${CPU_FLAGS}
        -T${LINKER_SCRIPT}
        -Wl,--gc-sections
        -Wl,-Map=eds_sensor_freertos.map
        -specs=nosys.specs
        -specs=nano.specs
        --static
    )
else()
    target_link_options(eds_sensor_freertos PRIVATE ${CPU_FLAGS})
endif()

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

set_target_properties(eds_sensor_freertos PROPERTIES
    OUTPUT_NAME "eds_sensor_freertos"
    SUFFIX ".elf"
)

add_custom_command(TARGET eds_sensor_freertos POST_BUILD
    COMMAND ${CMAKE_SIZE} eds_sensor_freertos.elf
    COMMENT "Binary size:"
)
