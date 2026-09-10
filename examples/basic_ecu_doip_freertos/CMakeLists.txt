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

# [SEC-BUILD-MODE-01 / issue #84] shared FreeRTOS build-mode option -- see
# cmake/eds_build_mode.cmake for why this is centralized rather than
# declared per-example (same lesson as cmake/eds_service_sources.cmake).
include(${DIAG_ROOT}/cmake/eds_build_mode.cmake)
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

# UDS service source list — set EDS_SERVICE_SRCS.
include(${DIAG_ROOT}/cmake/eds_service_sources.cmake)

add_executable(eds_freertos_doip
    # Application
    src/main.c

    # Board support (QEMU mps2-an386): reset vector + C runtime startup, and
    # the polled UART0 used for boot logging. Without startup.c there is no
    # vector table, no Reset_Handler and no --gc-sections root at all — the
    # linker warned "cannot find entry symbol Reset_Handler" and stripped the
    # entire image, producing an ELF with .text size 0.
    boards/qemu_cortex_m4/startup.c
    boards/qemu_cortex_m4/board_uart.c
    boards/qemu_cortex_m4/freertos_hooks.c

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
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/qemu_cortex_m4   # board_uart.h
    ${FREERTOS_DIR}/include
    ${FREERTOS_PORT_DIR}
    ${FREERTOS_CONFIG_DIR}
)

if(NOT LWIP_STUB)
    # -------------------------------------------------------------------
    # Real lwIP build (-DLWIP_DIR=/path/to/lwip)
    #
    # Vendoring follows this repo's existing convention for third-party
    # dependencies: fetch the upstream tree and point a *_DIR variable at
    # it, exactly as FREERTOS_DIR already works (and as west.yml does for
    # Zephyr). Nothing third-party is checked into the repo.
    #
    # Pinned upstream: lwIP STABLE-2_2_1_RELEASE. See the example README
    # and the compat-tests workflow for the exact clone step.
    #
    # Only the IPv4 + socket subset is compiled. IPv6, PPP, 6LoWPAN,
    # SLIP and the bridge interface are all deliberately excluded — DoIP
    # is IPv4/TCP over Ethernet and nothing else.
    # -------------------------------------------------------------------
    if(NOT EXISTS "${LWIP_DIR}/src/core/init.c")
        message(FATAL_ERROR
            "LWIP_DIR='${LWIP_DIR}' does not look like an lwIP source tree "
            "(expected src/core/init.c). Clone lwIP and point LWIP_DIR at it.")
    endif()

    set(LWIP_CORE_SRCS
        ${LWIP_DIR}/src/core/init.c
        ${LWIP_DIR}/src/core/def.c
        ${LWIP_DIR}/src/core/dns.c
        ${LWIP_DIR}/src/core/inet_chksum.c
        ${LWIP_DIR}/src/core/ip.c
        ${LWIP_DIR}/src/core/mem.c
        ${LWIP_DIR}/src/core/memp.c
        ${LWIP_DIR}/src/core/netif.c
        ${LWIP_DIR}/src/core/pbuf.c
        ${LWIP_DIR}/src/core/raw.c
        ${LWIP_DIR}/src/core/stats.c
        ${LWIP_DIR}/src/core/sys.c
        ${LWIP_DIR}/src/core/altcp.c
        ${LWIP_DIR}/src/core/altcp_alloc.c
        ${LWIP_DIR}/src/core/altcp_tcp.c
        ${LWIP_DIR}/src/core/tcp.c
        ${LWIP_DIR}/src/core/tcp_in.c
        ${LWIP_DIR}/src/core/tcp_out.c
        ${LWIP_DIR}/src/core/timeouts.c
        ${LWIP_DIR}/src/core/udp.c
        ${LWIP_DIR}/src/core/ipv4/acd.c
        ${LWIP_DIR}/src/core/ipv4/autoip.c
        ${LWIP_DIR}/src/core/ipv4/dhcp.c
        ${LWIP_DIR}/src/core/ipv4/etharp.c
        ${LWIP_DIR}/src/core/ipv4/icmp.c
        ${LWIP_DIR}/src/core/ipv4/igmp.c
        ${LWIP_DIR}/src/core/ipv4/ip4_frag.c
        ${LWIP_DIR}/src/core/ipv4/ip4.c
        ${LWIP_DIR}/src/core/ipv4/ip4_addr.c
        ${LWIP_DIR}/src/api/api_lib.c
        ${LWIP_DIR}/src/api/api_msg.c
        ${LWIP_DIR}/src/api/err.c
        ${LWIP_DIR}/src/api/if_api.c
        ${LWIP_DIR}/src/api/netbuf.c
        ${LWIP_DIR}/src/api/netdb.c
        ${LWIP_DIR}/src/api/netifapi.c
        ${LWIP_DIR}/src/api/sockets.c
        ${LWIP_DIR}/src/api/tcpip.c
        ${LWIP_DIR}/src/netif/ethernet.c
    )

    target_sources(eds_freertos_doip PRIVATE
        ${LWIP_CORE_SRCS}
        # lwIP port: FreeRTOS OS abstraction + LAN9118 driver
        boards/qemu_cortex_m4/lwip_port/sys_arch.c
        boards/qemu_cortex_m4/lwip_port/ethernetif_lan9118.c
        # Application-side bring-up (static IP, netif, RX poll task)
        src/eds_net.c
    )

    target_include_directories(eds_freertos_doip PRIVATE
        ${LWIP_DIR}/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/boards/qemu_cortex_m4/lwip_port
    )

    target_compile_definitions(eds_freertos_doip PRIVATE
        EDS_LWIP_REAL=1
    )

    # Upstream lwIP does not build warning-clean under this example's
    # -Wall -Wextra. Those warnings are not actionable here (we do not
    # patch vendored sources), so silence them for lwIP's own files only
    # — the EDS sources and the port layer keep the full warning set.
    set_source_files_properties(${LWIP_CORE_SRCS}
        PROPERTIES COMPILE_OPTIONS "-Wno-unused-but-set-variable;-Wno-type-limits;-Wno-implicit-fallthrough"
    )
else()
    # Compile-only stub — provide a minimal lwip/sockets.h shim.
    # This remains the default when LWIP_DIR is not set, unchanged.
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
    # Every other FreeRTOS example defines this (basic_ecu_freertos,
    # sensor_ecu_freertos, safeboot_freertos_ecu); this one never did.
    # platform/freertos/freertos_nvm.c guards its entire body on
    # `#if defined(EDS_PLATFORM_FREERTOS) && !defined(NVM_STORE_HOST_MOCK)`,
    # so without it the file compiled to an empty translation unit and
    # nvm_store_read/write/is_ready were undefined at link time. That went
    # unnoticed only because --gc-sections had been discarding every caller.
    EDS_PLATFORM_FREERTOS=1
    FREERTOS_DOIP_TASK_STACK_WORDS=1024
    FREERTOS_DOIP_TASK_PRIORITY=6
    # [SEC-BUILD-MODE-01 / issue #84] Give EDS_BUILD_IS_PRODUCTION the same
    # defined-and-1-or-0 signal Zephyr's Kconfig already gives it. The
    # $<BOOL:...> generator expression emits 1 when EDS_PLACEHOLDER_KEYS_ONLY
    # is ON, 0 when OFF -- production is declared with
    # -DEDS_PLACEHOLDER_KEYS_ONLY=OFF (see cmake/eds_build_mode.cmake).
    CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=$<BOOL:${EDS_PLACEHOLDER_KEYS_ONLY}>
)

# ---------------------------------------------------------------------------
# ARM Cortex-M4 flags
# ---------------------------------------------------------------------------

target_compile_options(eds_freertos_doip PRIVATE
    -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
    -Os -ffunction-sections -fdata-sections
    -Wall -Wextra -Wno-unused-parameter -Wno-sign-conversion
)

# -nostartfiles (not -nostdlib): boards/qemu_cortex_m4/startup.c supplies the
# reset vector and C runtime bring-up, so newlib's crt0 must stay out — but
# libc itself must stay IN. The compiler emits calls to memcpy/memset from
# ordinary struct assignment and array initialisation, and lwIP additionally
# needs memcmp/strlen/strcmp. Under the previous -nostdlib those were simply
# unresolved; the link only appeared to succeed because --gc-sections had
# already discarded every caller along with the rest of the image.
#
# --specs=nano.specs picks newlib-nano (smaller, no wide-char/locale bulk);
# --specs=nosys.specs supplies the bare-metal syscall stubs it expects.
target_link_options(eds_freertos_doip PRIVATE
    -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
    -Wl,--gc-sections
    -Wl,-Map=eds_freertos_doip.map
    -T "${CMAKE_CURRENT_SOURCE_DIR}/boards/qemu_cortex_m4/linker.ld"
    -nostartfiles
    --specs=nano.specs
    --specs=nosys.specs
)

set_target_properties(eds_freertos_doip PROPERTIES
    OUTPUT_NAME "eds_freertos_doip"
    SUFFIX ".elf"
)
