# examples/robot_joint_controller_ecu/CMakeLists.txt
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(robot_joint_controller_ecu
    VERSION     1.0.0
    LANGUAGES   C
    DESCRIPTION "Robot Joint Controller ECU — Xaloqi EDS robotics example"
)

set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

target_compile_options(app PRIVATE
    -Wall -Wextra -Wshadow -Wdouble-promotion -Wundef
    -Wno-unused-parameter -Wno-sign-conversion
    -fstack-usage
)

set(DIAG_ROOT           "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(DIAG_CONFIG_YAML    "${CMAKE_CURRENT_SOURCE_DIR}/diagnostics_config.yaml")
set(DIAG_CODEGEN_SCRIPT "${DIAG_ROOT}/tools/codegen.py")
set(DIAG_GENERATED_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/generated")
set(CODEGEN_SENTINEL    "${DIAG_GENERATED_DIR}/generated_config.h")

option(DIAG_SKIP_CODEGEN "Skip codegen (use existing generated/ files)" OFF)

if(NOT DIAG_SKIP_CODEGEN)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_FOUND)
        message(FATAL_ERROR "Python3 not found. Use -DDIAG_SKIP_CODEGEN=ON")
    endif()
    file(GLOB DIAG_TEMPLATES "${DIAG_ROOT}/tools/templates/*.j2")
    add_custom_command(
        OUTPUT   "${CODEGEN_SENTINEL}"
        COMMAND  "${Python3_EXECUTABLE}" "${DIAG_CODEGEN_SCRIPT}"
                 --config "${DIAG_CONFIG_YAML}"
                 --out    "${DIAG_GENERATED_DIR}"
                 --safety-wrappers --asil-level B --no-manifest
        DEPENDS  "${DIAG_CONFIG_YAML}" "${DIAG_CODEGEN_SCRIPT}" ${DIAG_TEMPLATES}
        COMMENT  "[robot_ecu] Generating sources..."
        VERBATIM
    )
    add_custom_target(run_codegen DEPENDS "${CODEGEN_SENTINEL}")
endif()

target_include_directories(app PRIVATE
    ${DIAG_ROOT}/core
    ${DIAG_ROOT}/core/uds_services
    ${DIAG_ROOT}/transport
    ${DIAG_ROOT}/config
    ${DIAG_ROOT}/platform
    ${DIAG_ROOT}/platform/zephyr
    ${DIAG_GENERATED_DIR}
)

target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c

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
    ${DIAG_ROOT}/core/uds_services/service_0x31.c
    ${DIAG_ROOT}/core/uds_services/service_0x34.c
    ${DIAG_ROOT}/core/uds_services/service_0x35.c
    ${DIAG_ROOT}/core/uds_services/service_0x36.c
    ${DIAG_ROOT}/core/uds_services/service_0x37.c
    ${DIAG_ROOT}/core/uds_services/service_0x3E.c
    ${DIAG_ROOT}/core/uds_services/service_0x85.c

    # ISO-TP + CAN
    ${DIAG_ROOT}/transport/isotp.c
    ${DIAG_ROOT}/transport/can_transport.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_can.c  # moved from transport/ — phase-housekeeping

    # Databases
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
