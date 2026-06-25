# =============================================================================
# Xaloqi EDS
# examples/basic_ecu_doip_freertos/CMakeLists.txt
#
# PURPOSE: Standalone CMake build for the FreeRTOS + DoIP example ECU (v1.6.0).
#
# Mirrors examples/basic_ecu_freertos/CMakeLists.txt exactly, with additions:
#   1. Transport/DoIP include path (-Itransport/doip, -Iplatform/freertos)
#   2. DoIP sources (freertos_lwip.c, platform_doip.c, doip_server.c)
#   3. EDS_DOIP_ONLY_BUILD=1 — disables isotp_init() in uds_init.c
#   4. No CAN sources (no isotp.c, can_transport.c, freertos_can.c)
#   5. LwIP required (customer provides lwip and its CMake integration)
#
# USAGE:
#   cmake -B build \
#     -DEDS_PLATFORM=freertos \
#     -DFREERTOS_DIR=/path/to/FreeRTOS-Kernel \
#     -DLWIP_DIR=/path/to/lwip \
#     -DFREERTOS_CONFIG_DIR=<dir containing FreeRTOSConfig.h> \
#     -GNinja .
#   ninja -C build
#
# CI (compile-only, ARM cross-compile):
#   Same as freertos-qemu but with -DEDS_DOIP_ONLY_BUILD=1.
#   LwIP is stubbed for compile testing — no runtime test in CI.
#
# SAFETY CONTEXT : Example application — not safety-assessed.
# SPDX-License-Identifier: Apache-2.0
# =============================================================================

cmake_minimum_required(VERSION 3.20)

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

project(xaloqi_eds_freertos_doip_basic_ecu C)

set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

# ---------------------------------------------------------------------------
# Required parameters
# ---------------------------------------------------------------------------

if(NOT DEFINED FREERTOS_DIR)
    message(FATAL_ERROR "FREERTOS_DIR must point to FreeRTOS-Kernel directory")
endif()
if(NOT DEFINED FREERTOS_CONFIG_DIR)
    set(FREERTOS_CONFIG_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/qemu_cortex_m4"
        CACHE PATH "Directory containing FreeRTOSConfig.h")
endif()
if(NOT DEFINED LWIP_DIR)
    message(STATUS "LWIP_DIR not set — using LwIP stub for compile testing only")
    set(LWIP_STUB ON)
endif()

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

set(DIAG_ROOT         "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(FREERTOS_PORT_DIR "${FREERTOS_DIR}/portable/GCC/ARM_CM4F")

# ---------------------------------------------------------------------------
# FreeRTOS sources (same subset as basic_ecu_freertos)
# ---------------------------------------------------------------------------

set(FREERTOS_SRCS
    "${FREERTOS_DIR}/tasks.c"
    "${FREERTOS_DIR}/list.c"
    "${FREERTOS_DIR}/queue.c"
    "${FREERTOS_DIR}/timers.c"
    "${FREERTOS_DIR}/portable/GCC/ARM_CM4F/port.c"
    "${FREERTOS_DIR}/portable/MemMang/heap_4.c"
)

# ---------------------------------------------------------------------------
# EDS sources
# ---------------------------------------------------------------------------

add_executable(eds_freertos_doip
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
    # Source list is centrally managed — add new SIDs in cmake/eds_service_sources.cmake.
    include(${DIAG_ROOT}/cmake/eds_service_sources.cmake)
    ${EDS_SERVICE_SRCS}
    ${DIAG_ROOT}/core/uds_transfer_ctx.c
    ${DIAG_ROOT}/platform/uds_flash_ops.c

    # DoIP transport (FreeRTOS + LwIP binding)
    ${DIAG_ROOT}/transport/doip/doip_server.c
    ${DIAG_ROOT}/transport/doip/freertos_lwip.c
    ${DIAG_ROOT}/platform/freertos/platform_doip.c

    # NOTE: No isotp.c / can_transport.c / freertos_can.c — DoIP-only build

    # FreeRTOS platform (minimal: NVM + platform init, no CAN task)
    ${DIAG_ROOT}/platform/freertos/freertos_platform_api.c
    ${DIAG_ROOT}/platform/freertos/freertos_nvm.c

    # Config databases
    ${DIAG_ROOT}/config/did_database.c
    ${DIAG_ROOT}/config/dtc_database.c
    ${DIAG_ROOT}/config/routine_database.c
    ${DIAG_ROOT}/config/dtc_mirror.c

    # Generated sources
    generated/did_handlers.c
    generated/uds_init.c
    generated/did_safety_wrappers.c
    generated/routine_handlers.c

    # FreeRTOS kernel
    ${FREERTOS_SRCS}
)

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------

target_include_directories(eds_freertos_doip PRIVATE
    ${DIAG_ROOT}/core
    ${DIAG_ROOT}/core/uds_services
    ${DIAG_ROOT}/transport
    ${DIAG_ROOT}/transport/doip          # doip_server.h, freertos_lwip.h
    ${DIAG_ROOT}/config
    ${DIAG_ROOT}/platform
    ${DIAG_ROOT}/platform/freertos       # platform_api.h, platform_doip.h, freertos_lwip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
    ${FREERTOS_DIR}/include
    ${FREERTOS_PORT_DIR}
    ${FREERTOS_CONFIG_DIR}
)

if(NOT LWIP_STUB)
    target_include_directories(eds_freertos_doip PRIVATE
        ${LWIP_DIR}/src/include
        ${LWIP_DIR}/src/include/ipv4
    )
else()
    # Compile-only stub — provide a minimal lwip/sockets.h shim
    target_include_directories(eds_freertos_doip PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/boards/qemu_cortex_m4/lwip_stub
    )
endif()

# ---------------------------------------------------------------------------
# Compile definitions
# ---------------------------------------------------------------------------

target_compile_definitions(eds_freertos_doip PRIVATE
    EDS_MSG_BUF_MAX_STACK_BYTES=8192
    EDS_DOIP_ONLY_BUILD=1
    FREERTOS_DOIP_TASK_STACK_WORDS=1024
    FREERTOS_DOIP_TASK_PRIORITY=6
)

# ---------------------------------------------------------------------------
# ARM Cortex-M4 flags
# ---------------------------------------------------------------------------

target_compile_options(eds_freertos_doip PRIVATE
    -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
    -Os -ffunction-sections -fdata-sections
    -Wall -Wextra -Wno-unused-parameter -Wno-sign-conversion
)

target_link_options(eds_freertos_doip PRIVATE
    -mcpu=cortex-m4 -mthumb
    -Wl,--gc-sections
    -T "${CMAKE_CURRENT_SOURCE_DIR}/boards/qemu_cortex_m4/linker.ld"
    -nostdlib
    --specs=nosys.specs
)

set_target_properties(eds_freertos_doip PROPERTIES
    OUTPUT_NAME "eds_freertos_doip"
    SUFFIX ".elf"
)
