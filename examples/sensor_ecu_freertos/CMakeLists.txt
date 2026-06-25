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
# CI JOB: sensor-freertos-qemu (added to .github/workflows/ci.yml)
#   Builds this example with QEMU ARM Cortex-M4 and verifies the ELF is
#   produced. Same pattern as the freertos-qemu job for basic_ecu_freertos.
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

if(NOT DEFINED BOARD)
    set(BOARD "qemu_cortex_m4")
endif()

# ---------------------------------------------------------------------------
# Repo root (two levels up)
# ---------------------------------------------------------------------------

set(DIAG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# ---------------------------------------------------------------------------
# Board-specific compiler flags
# ---------------------------------------------------------------------------

if(BOARD STREQUAL "qemu_cortex_m4" OR BOARD STREQUAL "stm32f4")
    set(CPU_FLAGS -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
    set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/boards/qemu_cortex_m4/linker.ld")
elseif(BOARD STREQUAL "stm32h7")
    set(CPU_FLAGS -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard)
    set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/boards/stm32h7/linker.ld")
else()
    message(WARNING "Unknown BOARD '${BOARD}' — using Cortex-M4 flags.")
    set(CPU_FLAGS -mcpu=cortex-m4 -mthumb)
endif()

# ---------------------------------------------------------------------------
# FreeRTOSConfig.h location
# ---------------------------------------------------------------------------

if(NOT DEFINED FREERTOS_CONFIG_DIR)
    set(FREERTOS_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/qemu_cortex_m4")
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
        "    --safety-wrappers --asil-level B --no-manifest")
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------

add_executable(eds_sensor_freertos
    # Application
    src/main.c
    src/sensor_monitor_freertos.c
    # NOTE: did_handlers_impl.c is NOT listed here.
    # The CI step "Install sensor DID implementations" copies it to
    # generated/did_handlers.c before the build, so it is compiled once
    # via the generated sources block below — avoiding duplicate symbols.

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

    # Service handlers
    ${DIAG_ROOT}/core/uds_services/service_registration.c
    ${DIAG_ROOT}/core/uds_services/service_0x10.c
    ${DIAG_ROOT}/core/uds_services/service_0x11.c
    ${DIAG_ROOT}/core/uds_services/service_0x14.c
    ${DIAG_ROOT}/core/uds_services/service_0x19.c
    ${DIAG_ROOT}/core/uds_services/service_0x22.c
    ${DIAG_ROOT}/core/uds_services/service_0x27.c
    ${DIAG_ROOT}/core/uds_services/service_0x28.c
    ${DIAG_ROOT}/core/uds_services/service_0x2E.c
    ${DIAG_ROOT}/core/uds_services/service_0x2F.c
    ${DIAG_ROOT}/core/uds_services/service_0x31.c
    ${DIAG_ROOT}/core/uds_services/service_0x34.c
    ${DIAG_ROOT}/core/uds_services/service_0x35.c
    ${DIAG_ROOT}/core/uds_services/service_0x36.c
    ${DIAG_ROOT}/core/uds_services/service_0x37.c
    ${DIAG_ROOT}/core/uds_services/service_0x3E.c
    ${DIAG_ROOT}/core/uds_services/service_0x85.c

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

    # Generated sources
    # did_handlers.c contains the real sensor implementations — installed by
    # the CI step "Install sensor DID implementations" which copies
    # src/did_handlers_impl.c here after codegen runs.
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
