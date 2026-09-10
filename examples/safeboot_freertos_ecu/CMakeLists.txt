# =============================================================================
# Xaloqi EDS
# examples/safeboot_freertos_ecu/CMakeLists.txt
#
# PURPOSE: Standalone CMake build for the FreeRTOS OTA DFU example on
#          STM32H743ZI (safeboot_freertos_ecu).
#
#          FreeRTOS companion to examples/safeboot_ecu/ (Zephyr + MCUboot).
#          Uses STM32H743ZI dual-bank flash for OTA staging without MCUboot.
#
# FLASH BACKEND:
#   When -DBOARD=nucleo_h743zi and STM32H7xx is defined (by the STM32 HAL
#   headers), freertos_flash_ops.c uses HAL_FLASH_* (real hardware).
#   When -DBOARD=qemu_cortex_m4 (CI default), the RAM stub is used
#   automatically — no STM32 HAL required.
#
#   To build for real hardware you must provide the STM32H7 HAL:
#     -DSTM32_HAL_DIR=/path/to/STM32H7xx_HAL_Driver
#   and set -DBOARD=nucleo_h743zi.
#
# USAGE (CI — compile only, QEMU Cortex-M4):
#   cmake -B build \
#     -DEDS_PLATFORM=freertos \
#     -DFREERTOS_DIR=/path/to/FreeRTOS-Kernel \
#     -DBOARD=qemu_cortex_m4 \
#     -GNinja .
#   ninja -C build
#
# USAGE (hardware — STM32H743ZI):
#   cmake -B build \
#     -DEDS_PLATFORM=freertos \
#     -DFREERTOS_DIR=/path/to/FreeRTOS-Kernel \
#     -DBOARD=nucleo_h743zi \
#     -DSTM32_HAL_DIR=/path/to/STM32H7xx_HAL_Driver \
#     -GNinja .
#   ninja -C build
#
# FLASH LAYOUT (nucleo_h743zi):
#   Bank 1  0x08000000 – 0x080FFFFF  1 MB   running application
#   Bank 2  0x08100000 – 0x081DFFFF  896 KB  OTA staging area
#   NVM     0x081E0000 – 0x081FFFFF  128 KB  last sector (reserved)
#
# CONTRIBUTOR: Flash driver based on work by chenyurong22 (Siemens) — issue #28.
#
# SAFETY CONTEXT : Example application — not safety-assessed.
# SPDX-License-Identifier: Apache-2.0
# =============================================================================

cmake_minimum_required(VERSION 3.20)

# Bare-metal cross-compilation — skip CMake's link-test (no OS syscalls).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

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

project(xaloqi_eds_safeboot_freertos C)

set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

# ---------------------------------------------------------------------------
# Required parameters
# ---------------------------------------------------------------------------


if(NOT DEFINED FREERTOS_DIR)
    message(FATAL_ERROR "FREERTOS_DIR must point to FreeRTOS-Kernel directory")
endif()

if(NOT DEFINED BOARD)
    set(BOARD "qemu_cortex_m4")
endif()

if(NOT DEFINED FREERTOS_CONFIG_DIR)
    set(FREERTOS_CONFIG_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/qemu_cortex_m4"
        CACHE PATH "Directory containing FreeRTOSConfig.h")
endif()

# ---------------------------------------------------------------------------
# Board-specific flags and linker script
# ---------------------------------------------------------------------------

# Board support sources: reset vector + C runtime startup, FreeRTOS hooks and
# boot log. Without a vector table ENTRY(Reset_Handler) has no root and
# --gc-sections deletes the whole image (EDS issue #268).
#
# nucleo_h743zi does NOT share the QEMU startup: it needs the STM32H743's own
# flash base (0x08000000) and its full 150-entry IRQ table, and it needs two
# extra FreeRTOS hooks its stricter FreeRTOSConfig.h demands.
set(EDS_COMMON_BOARDS "${CMAKE_CURRENT_SOURCE_DIR}/../common/boards")

if(BOARD STREQUAL "nucleo_h743zi")
    set(CPU_FLAGS -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard)
    set(FREERTOS_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/nucleo_h743zi")
    set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/boards/nucleo_h743zi/linker.ld")
    set(FREERTOS_PORT_SUBDIR "GCC/ARM_CM7/r0p1")
    set(BOARD_SRCS
        ${CMAKE_CURRENT_SOURCE_DIR}/boards/nucleo_h743zi/startup.c
        ${CMAKE_CURRENT_SOURCE_DIR}/boards/nucleo_h743zi/freertos_hooks.c
        ${CMAKE_CURRENT_SOURCE_DIR}/boards/nucleo_h743zi/board_log.c
        # HAL_GetTick() for the STM32 flash HAL, backed by the FreeRTOS tick.
        # Compiles to nothing unless STM32H7xx/STM32H743xx is defined.
        ${CMAKE_CURRENT_SOURCE_DIR}/boards/nucleo_h743zi/hal_timebase.c
    )
    # Without the STM32 HAL, freertos_flash_ops.c falls back to its RAM stub,
    # which is a 896 KB static buffer standing in for the OTA staging slot.
    # That cannot fit the H743's 128 KB DTCM: the link fails with
    # "region `DTCM' overflowed by 910528 bytes". Say so here rather than
    # leaving the integrator to decode a linker overflow. (Before EDS #268
    # this combination appeared to build, because the image was empty.)
    if(NOT DEFINED STM32_HAL_DIR)
        message(FATAL_ERROR
            "BOARD=nucleo_h743zi needs the STM32H7 flash HAL.\n"
            "Pass -DSTM32_HAL_DIR=/path/to/STM32H7xx_HAL_Driver.\n"
            "Without it freertos_flash_ops.c compiles its 896 KB RAM stub, "
            "which does not fit this MCU's 128 KB DTCM. For a HAL-free "
            "compile check use -DBOARD=qemu_cortex_m4.")
    endif()
else()
    # qemu_cortex_m4 — default for CI
    set(CPU_FLAGS -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
    set(FREERTOS_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/qemu_cortex_m4")
    set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/boards/qemu_cortex_m4/linker.ld")
    set(FREERTOS_PORT_SUBDIR "GCC/ARM_CM4F")
    set(BOARD_SRCS
        ${EDS_COMMON_BOARDS}/qemu_cortex_m4/startup.c
        ${EDS_COMMON_BOARDS}/qemu_cortex_m4/freertos_hooks.c
        ${EDS_COMMON_BOARDS}/qemu_cortex_m4/board_log.c
    )
endif()

# ---------------------------------------------------------------------------
# FreeRTOS port
# ---------------------------------------------------------------------------

set(FREERTOS_PORT_DIR "${FREERTOS_DIR}/portable/${FREERTOS_PORT_SUBDIR}")
if(NOT EXISTS "${FREERTOS_PORT_DIR}/port.c")
    # Fall back to CM4F if CM7 port not present (some FreeRTOS distributions)
    set(FREERTOS_PORT_DIR "${FREERTOS_DIR}/portable/GCC/ARM_CM4F")
    if(NOT EXISTS "${FREERTOS_PORT_DIR}/port.c")
        message(FATAL_ERROR "Cannot find FreeRTOS port in ${FREERTOS_DIR}/portable")
    endif()
endif()

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

set(DIAG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# [SEC-BUILD-MODE-01 / issue #84] shared FreeRTOS build-mode option -- see
# cmake/eds_build_mode.cmake for why this is centralized rather than
# declared per-example (same lesson as cmake/eds_service_sources.cmake).
include(${DIAG_ROOT}/cmake/eds_build_mode.cmake)

# ---------------------------------------------------------------------------
# FreeRTOS kernel sources
# ---------------------------------------------------------------------------

set(FREERTOS_SRCS
    "${FREERTOS_DIR}/tasks.c"
    "${FREERTOS_DIR}/list.c"
    "${FREERTOS_DIR}/queue.c"
    "${FREERTOS_DIR}/timers.c"
    "${FREERTOS_PORT_DIR}/port.c"
    "${FREERTOS_DIR}/portable/MemMang/heap_4.c"
)

# UDS service source list — set EDS_SERVICE_SRCS.
include(${DIAG_ROOT}/cmake/eds_service_sources.cmake)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------

add_executable(eds_safeboot_freertos
    # Board support: reset vector, C runtime startup, FreeRTOS hooks, boot log.
    ${BOARD_SRCS}

    # Application
    src/main.c

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
    # UDS service handlers (all 16 SIDs + dispatch table).
    ${EDS_SERVICE_SRCS}
    ${DIAG_ROOT}/core/uds_transfer_ctx.c

    # Transport
    ${DIAG_ROOT}/transport/isotp.c
    ${DIAG_ROOT}/transport/can_transport.c

    # Config databases
    ${DIAG_ROOT}/config/did_database.c
    ${DIAG_ROOT}/config/dtc_database.c
    ${DIAG_ROOT}/config/routine_database.c
    ${DIAG_ROOT}/config/dtc_mirror.c

    # FreeRTOS platform HAL
    ${DIAG_ROOT}/platform/freertos/freertos_platform_api.c
    ${DIAG_ROOT}/platform/freertos/freertos_can.c
    ${DIAG_ROOT}/platform/freertos/freertos_nvm.c
    ${DIAG_ROOT}/platform/uds_flash_ops.c
    ${DIAG_ROOT}/platform/freertos/freertos_flash_ops.c

    # Generated sources
    generated/uds_init.c
    generated/did_handlers.c
    generated/did_safety_wrappers.c
    generated/routine_handlers.c

    # FreeRTOS kernel
    ${FREERTOS_SRCS}
)

# ---------------------------------------------------------------------------
# Optional STM32H7 HAL (real hardware only)
# ---------------------------------------------------------------------------

if(DEFINED STM32_HAL_DIR)
    target_sources(eds_safeboot_freertos PRIVATE
        "${STM32_HAL_DIR}/Src/stm32h7xx_hal_flash.c"
        "${STM32_HAL_DIR}/Src/stm32h7xx_hal_flash_ex.c"
    )
    target_include_directories(eds_safeboot_freertos PRIVATE
        "${STM32_HAL_DIR}/Inc"
    )
    target_compile_definitions(eds_safeboot_freertos PRIVATE
        STM32H743xx
        USE_HAL_DRIVER
    )
    message(STATUS "STM32H7 HAL enabled: ${STM32_HAL_DIR}")
else()
    message(STATUS "STM32H7 HAL not set — using RAM stub (CI/QEMU mode)")
endif()

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------

target_include_directories(eds_safeboot_freertos PRIVATE
    ${DIAG_ROOT}/core
    ${DIAG_ROOT}/core/uds_services
    ${DIAG_ROOT}/transport
    ${DIAG_ROOT}/config
    ${DIAG_ROOT}/platform
    ${DIAG_ROOT}/platform/freertos
    ${EDS_COMMON_BOARDS}
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
    ${FREERTOS_DIR}/include
    ${FREERTOS_PORT_DIR}
    ${FREERTOS_CONFIG_DIR}
)

# ---------------------------------------------------------------------------
# Compile definitions
# ---------------------------------------------------------------------------

target_compile_definitions(eds_safeboot_freertos PRIVATE
    EDS_PLATFORM_FREERTOS=1
    EDS_MSG_BUF_MAX_STACK_BYTES=8192
    # [SEC-BUILD-MODE-01 / issue #84] Give EDS_BUILD_IS_PRODUCTION the same
    # defined-and-1-or-0 signal Zephyr's Kconfig already gives it. The
    # $<BOOL:...> generator expression emits 1 when EDS_PLACEHOLDER_KEYS_ONLY
    # is ON, 0 when OFF -- production is declared with
    # -DEDS_PLACEHOLDER_KEYS_ONLY=OFF (see cmake/eds_build_mode.cmake).
    CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=$<BOOL:${EDS_PLACEHOLDER_KEYS_ONLY}>
)

# ---------------------------------------------------------------------------
# Compile options
# ---------------------------------------------------------------------------

target_compile_options(eds_safeboot_freertos PRIVATE
    ${CPU_FLAGS}
    -std=c11
    -Wall
    -Wextra
    -Wno-unused-parameter
    # GCC 14 promotes uint16_t/size_t callback mismatches in freertos_platform_api.c
    # to errors. That mismatch is pre-existing (not in this example's code).
    # CI uses GCC 11 (ubuntu-22.04) where this is only a warning.
    -Wno-incompatible-pointer-types
    -Os
    -ffunction-sections
    -fdata-sections
)

# ---------------------------------------------------------------------------
# Linker options
# ---------------------------------------------------------------------------

if(EXISTS "${LINKER_SCRIPT}")
    target_link_options(eds_safeboot_freertos PRIVATE
        ${CPU_FLAGS}
        -T${LINKER_SCRIPT}
        -Wl,--gc-sections
        -Wl,-Map=eds_safeboot_freertos.map
        -specs=nosys.specs
        -specs=nano.specs
        --static
    )
else()
    target_link_options(eds_safeboot_freertos PRIVATE ${CPU_FLAGS})
endif()

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

set_target_properties(eds_safeboot_freertos PROPERTIES
    OUTPUT_NAME "eds_safeboot_freertos"
    SUFFIX ".elf"
)

add_custom_command(TARGET eds_safeboot_freertos POST_BUILD
    COMMAND ${CMAKE_SIZE} eds_safeboot_freertos.elf
    COMMENT "Binary size:"
)
