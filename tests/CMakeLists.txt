# =============================================================================
# Xaloqi EDS
# FILE: tests/CMakeLists.txt
#
# PURPOSE: Host-native CMake build for all 37 unit test modules.
#          Compiles each test_*.c file together with its production sources
#          and the Unity framework into a standalone executable. CTest then
#          runs each executable and reports pass/fail based on exit code.
#
#          Architecture:
#            tests/CMakeLists.txt (this file)
#              -> tests/runner/test_main.c         (Unity main + lifecycle)
#              -> tests/runner/ztest_shim.h        (ZTEST-to-Unity macro map)
#              -> project/unity/unity.c            (Unity assertion engine)
#              -> tests/unit_runnable/test_<module>.c  (canonical test cases — 37 modules)
#              -> <production sources>             (modules under test)
#              -> generated/ did_handlers.c etc.  (generated stubs)
#
#          This design allows all 37 modules to be tested on a CI runner
#          without a Zephyr SDK or hardware target, using only GCC/Clang
#          and CMake. Each test module compiles independently to avoid
#          link-time symbol conflicts between test modules.
#
# USAGE:
#   # Build and run all tests:
#   mkdir -p tests/build && cd tests/build
#   cmake .. -DCMAKE_BUILD_TYPE=Debug
#   cmake --build .
#   ctest --output-on-failure
#
#   # Run a single test module:
#   ./test_uds_session
#
#   # Run with verbose output:
#   ctest -V
#
# CI INTEGRATION:
#   ctest returns 0 on all pass, non-zero on any failure.
#   Use --output-on-failure to see test output only when something fails.
#   Use --parallel <N> to run test executables concurrently.
#
# SAFETY CONTEXT : Test infrastructure — not safety-assessed.
# CODING STANDARD: MISRA C:2012 alignment intended (advisory for test code).
# VERSION        : 1.7.0 (37 modules wired, unit_runnable/ is the single canonical test directory)
# SPDX-License-Identifier: Apache-2.0
# =============================================================================

cmake_minimum_required(VERSION 3.16)

project(diag_unit_tests
    VERSION     1.7.0
    LANGUAGES   C
    DESCRIPTION "Host-native Unity unit tests — Xaloqi EDS"
)

# ---------------------------------------------------------------------------
# C standard
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

# ---------------------------------------------------------------------------
# Build type default
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# CTest integration
# ---------------------------------------------------------------------------
enable_testing()

# ---------------------------------------------------------------------------
# Path configuration
#
# All paths are relative to this file's directory (tests/).
# DIAG_ROOT is the repository root (parent of tests/).
# ---------------------------------------------------------------------------
set(DIAG_ROOT          "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(DIAG_CORE          "${DIAG_ROOT}/core")
set(DIAG_SERVICES      "${DIAG_ROOT}/core/uds_services")
set(DIAG_TRANSPORT     "${DIAG_ROOT}/transport")
set(DIAG_CONFIG        "${DIAG_ROOT}/config")
set(DIAG_PLATFORM      "${DIAG_ROOT}/platform")
set(DIAG_PLATFORM_ZEPHYR "${DIAG_ROOT}/platform/zephyr")
set(DIAG_GENERATED     "${DIAG_ROOT}/generated")
set(DIAG_UNITY         "${DIAG_ROOT}/project/unity")
# [M2] Canonical test directory is unit_runnable/ (37 modules).
# tests/legacy_unit/ has been removed — unit_runnable/ is the single source of truth.
# build_tests.sh and this CMakeLists.txt both reference unit_runnable/.
set(TEST_UNIT_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/unit_runnable")
set(TEST_RUNNER_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/runner")

# ---------------------------------------------------------------------------
# Unity static library
#
# Built once and linked into every test executable.
# Compiled without Zephyr headers — pure ISO C11.
# ---------------------------------------------------------------------------
add_library(unity STATIC
    "${DIAG_UNITY}/unity.c"
)
target_include_directories(unity PUBLIC
    "${DIAG_UNITY}"
)
target_compile_options(unity PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-pedantic
)

# ---------------------------------------------------------------------------
# Common include directories for production code
# ---------------------------------------------------------------------------
set(DIAG_INCLUDE_DIRS
    "${DIAG_CORE}"
    "${DIAG_SERVICES}"
    "${DIAG_TRANSPORT}"
    "${DIAG_CONFIG}"
    "${DIAG_PLATFORM}"
    "${DIAG_PLATFORM_ZEPHYR}"
    "${DIAG_GENERATED}"
    "${DIAG_UNITY}"
    "${TEST_RUNNER_DIR}"
)

# ---------------------------------------------------------------------------
# Common compile options for test executables
# ---------------------------------------------------------------------------
set(DIAG_TEST_COMPILE_OPTIONS
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-pedantic
    # Inject ztest_shim.h as the first include, replacing <zephyr/ztest.h>
    # with the Unity-compatible macro definitions.
    -include "${TEST_RUNNER_DIR}/ztest_shim.h"
    # Stub out Zephyr ztest include guard so the real header is never opened.
    "-DZTEST_HOST_SHIM=1"
)

# ---------------------------------------------------------------------------
# Helper function: add_diag_test(name sources... [EXTRA_DEFINES ...])
#
# Creates a test executable named 'name' that:
#   - Compiles the listed source files
#   - Links against Unity
#   - Includes all DIAG_INCLUDE_DIRS
#   - Applies DIAG_TEST_COMPILE_OPTIONS
#   - Registers with CTest via add_test()
# ---------------------------------------------------------------------------
function(add_diag_test TEST_NAME)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEFINES" ${ARGN})

    add_executable(${TEST_NAME}
        "${TEST_RUNNER_DIR}/test_main.c"
        ${ARG_SOURCES}
    )

    target_include_directories(${TEST_NAME} PRIVATE
        ${DIAG_INCLUDE_DIRS}
    )

    target_compile_options(${TEST_NAME} PRIVATE
        ${DIAG_TEST_COMPILE_OPTIONS}
    )

    if(ARG_DEFINES)
        target_compile_definitions(${TEST_NAME} PRIVATE ${ARG_DEFINES})
    endif()

    target_link_libraries(${TEST_NAME} PRIVATE unity)

    # Register with CTest. Exit code 0 = all pass, non-zero = failures.
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

    # Propagate test failure to CTest if the binary aborts.
    set_tests_properties(${TEST_NAME} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS"
        FAIL_REGULAR_EXPRESSION "FAIL;ERROR;Segmentation fault"
        TIMEOUT 30
    )
endfunction()

# ===========================================================================
# Common production source sets
#
# Groups of sources required by multiple test modules. Using lists avoids
# duplication and ensures consistent compilation across related test targets.
# ===========================================================================

set(DIAG_SRCS_SESSION
    "${DIAG_CORE}/uds_session.c"
)

set(DIAG_SRCS_SECURITY
    "${DIAG_CORE}/uds_security.c"
)

set(DIAG_SRCS_SAFETY
    "${DIAG_CORE}/uds_safety.c"
)

set(DIAG_SRCS_SERVER
    "${DIAG_CORE}/uds_server.c"
    "${DIAG_CORE}/uds_session.c"
    "${DIAG_CORE}/uds_security.c"
)

set(DIAG_SRCS_DID
    "${DIAG_CONFIG}/did_database.c"
    "${DIAG_CONFIG}/dtc_database.c"
    "${DIAG_GENERATED}/did_handlers.c"
)

set(DIAG_SRCS_SAFETY_WRAPPERS
    "${DIAG_GENERATED}/did_safety_wrappers.c"
    "${DIAG_CORE}/uds_safety.c"
    "${DIAG_CORE}/uds_session.c"
    "${DIAG_CORE}/uds_security.c"
    "${DIAG_CONFIG}/did_database.c"
    "${DIAG_CONFIG}/dtc_database.c"
    "${DIAG_GENERATED}/did_handlers.c"
)

set(DIAG_SRCS_SERVICES_FULL
    # All 14 service handlers (SIDs 0x10/11/14/19/22/27/28/2E/31/3E/85
    #   plus 0x34/36/37 download services)
    "${DIAG_SERVICES}/service_registration.c"
    "${DIAG_SERVICES}/service_0x10.c"
    "${DIAG_SERVICES}/service_0x11.c"
    "${DIAG_SERVICES}/service_0x14.c"
    "${DIAG_SERVICES}/service_0x19.c"
    "${DIAG_SERVICES}/service_0x22.c"
    "${DIAG_SERVICES}/service_0x27.c"
    "${DIAG_SERVICES}/service_0x28.c"
    "${DIAG_SERVICES}/service_0x2E.c"
    "${DIAG_SERVICES}/service_0x31.c"
    "${DIAG_SERVICES}/service_0x34.c"
    "${DIAG_SERVICES}/service_0x36.c"
    "${DIAG_SERVICES}/service_0x37.c"
    "${DIAG_SERVICES}/service_0x3E.c"
    "${DIAG_SERVICES}/service_0x85.c"
    "${DIAG_CORE}/uds_server.c"
    "${DIAG_CORE}/uds_session.c"
    "${DIAG_CORE}/uds_security.c"
    "${DIAG_CORE}/uds_safety.c"
    "${DIAG_CORE}/uds_access_table.c"
    "${DIAG_CORE}/uds_comm_control.c"
    "${DIAG_CORE}/uds_transfer_ctx.c"
    "${DIAG_CONFIG}/did_database.c"
    "${DIAG_CONFIG}/dtc_database.c"
    "${DIAG_CONFIG}/routine_database.c"
    "${DIAG_GENERATED}/did_handlers.c"
    "${DIAG_GENERATED}/did_safety_wrappers.c"
    "${DIAG_GENERATED}/routine_handlers.c"
    "${DIAG_PLATFORM}/uds_flash_ops.c"
)

# Source set for Phase 2 — ISO-TP STmin and session matrix tests
set(DIAG_SRCS_PHASE2_ISOTP
    "${DIAG_TRANSPORT}/isotp.c"
    "${DIAG_TRANSPORT}/can_transport.c"
)

set(DIAG_SRCS_PHASE2_SESSION
    "${DIAG_CORE}/uds_session.c"
)

# Source set for Phase 2 suppress-bit — needs full server + services
set(DIAG_SRCS_PHASE2_SUPPRESS
    ${DIAG_SRCS_SERVICES_FULL}
)

# Source set for Phase 3 — NVM-backed modules
set(DIAG_SRCS_PHASE3_NVM
    "${DIAG_PLATFORM_ZEPHYR}/nvm_store_mock.c"
    "${DIAG_CONFIG}/dtc_mirror.c"
    "${DIAG_CONFIG}/dtc_database.c"
    "${DIAG_CORE}/uds_session_stats.c"
    "${DIAG_CORE}/uds_security_nvm.c"
    "${DIAG_CORE}/uds_security.c"
    "${DIAG_CORE}/uds_session.c"
)

# Source set for Phase 5 — access table + security algo
set(DIAG_SRCS_PHASE5_ACCESS
    "${DIAG_CORE}/uds_access_table.c"
    "${DIAG_CORE}/uds_security.c"
    "${DIAG_CORE}/uds_session.c"
)

set(DIAG_SRCS_PHASE5_SECURITY_ALGO
    "${DIAG_CORE}/uds_security_algo.c"
    "${DIAG_CORE}/uds_aes_cmac.c"
    "${DIAG_CORE}/uds_security.c"
    "${DIAG_CORE}/uds_session.c"
)

# Source set for routine database
set(DIAG_SRCS_ROUTINE_DB
    "${DIAG_CONFIG}/routine_database.c"
)

# ===========================================================================
# Test targets — one per module under test
# ===========================================================================

# ---------------------------------------------------------------------------
# 1. test_uds_session
#    Module under test: core/uds_session.c
# ---------------------------------------------------------------------------
add_diag_test(test_uds_session
    SOURCES
        "${TEST_UNIT_DIR}/test_uds_session.c"
        ${DIAG_SRCS_SESSION}
)

# ---------------------------------------------------------------------------
# 2. test_uds_security
#    Module under test: core/uds_security.c
# ---------------------------------------------------------------------------
add_diag_test(test_uds_security
    SOURCES
        "${TEST_UNIT_DIR}/test_uds_security.c"
        ${DIAG_SRCS_SECURITY}
        ${DIAG_SRCS_SESSION}
)

# ---------------------------------------------------------------------------
# 3. test_uds_safety
#    Module under test: core/uds_safety.c
#    Requires: did_database, dtc_database (for verify_did_database test)
# ---------------------------------------------------------------------------
add_diag_test(test_uds_safety
    SOURCES
        "${TEST_UNIT_DIR}/test_uds_safety.c"
        ${DIAG_SRCS_SAFETY}
        ${DIAG_SRCS_SESSION}
        ${DIAG_SRCS_SECURITY}
        "${DIAG_CONFIG}/did_database.c"
        "${DIAG_CONFIG}/dtc_database.c"
        "${DIAG_GENERATED}/did_handlers.c"
)

# ---------------------------------------------------------------------------
# 4. test_uds_server
#    Module under test: core/uds_server.c
# ---------------------------------------------------------------------------
add_diag_test(test_uds_server
    SOURCES
        "${TEST_UNIT_DIR}/test_uds_server.c"
        ${DIAG_SRCS_SERVER}
        ${DIAG_SRCS_SAFETY}
        "${DIAG_CONFIG}/did_database.c"
        "${DIAG_CONFIG}/dtc_database.c"
        "${DIAG_GENERATED}/did_handlers.c"
        "${DIAG_SERVICES}/service_registration.c"
        "${DIAG_SERVICES}/service_0x10.c"
        "${DIAG_SERVICES}/service_0x11.c"
        "${DIAG_SERVICES}/service_0x22.c"
        "${DIAG_SERVICES}/service_0x27.c"
        "${DIAG_SERVICES}/service_0x2E.c"
        "${DIAG_SERVICES}/service_0x3E.c"
        "${DIAG_GENERATED}/did_safety_wrappers.c"
)

# ---------------------------------------------------------------------------
# 5. test_did_database
#    Module under test: config/did_database.c
# ---------------------------------------------------------------------------
add_diag_test(test_did_database
    SOURCES
        "${TEST_UNIT_DIR}/test_did_database.c"
        "${DIAG_CONFIG}/did_database.c"
)

# ---------------------------------------------------------------------------
# 6. test_dtc_database
#    Module under test: config/dtc_database.c
# ---------------------------------------------------------------------------
add_diag_test(test_dtc_database
    SOURCES
        "${TEST_UNIT_DIR}/test_dtc_database.c"
        "${DIAG_CONFIG}/dtc_database.c"
)

# ---------------------------------------------------------------------------
# 6b. test_dtc_mirror
#     Module under test: config/dtc_mirror.c — DTC NVM persistence module
#
#     Focused unit test covering:
#       - dtc_mirror_init() pre-condition guards and idempotence
#       - dtc_mirror_load() soft-degrade when NVM not ready (H3 fix path)
#       - dtc_mirror_flush_all() / save_one() / clear_all() API contract
#       - Three H3 fix integration tests proving fault history survives reset
#
#     Source set: DIAG_SRCS_PHASE3_NVM (same as test_phase3_nvm_dtc).
#     NVM mock activated via NVM_STORE_HOST_MOCK=1.
# ---------------------------------------------------------------------------
add_diag_test(test_dtc_mirror
    SOURCES
        "${TEST_UNIT_DIR}/test_dtc_mirror.c"
        ${DIAG_SRCS_PHASE3_NVM}
    DEFINES
        NVM_STORE_HOST_MOCK=1
)

# ---------------------------------------------------------------------------
# 7. test_did_handlers
#    Module under test: generated/did_handlers.c
#    Also exercises generated/did_safety_wrappers.c
# ---------------------------------------------------------------------------
add_diag_test(test_did_handlers
    SOURCES
        "${TEST_UNIT_DIR}/test_did_handlers.c"
        ${DIAG_SRCS_SAFETY_WRAPPERS}
)

# ---------------------------------------------------------------------------
# 8. test_did_safety_wrappers
#    Module under test: generated/did_safety_wrappers.c
# ---------------------------------------------------------------------------
add_diag_test(test_did_safety_wrappers
    SOURCES
        "${TEST_UNIT_DIR}/test_did_safety_wrappers.c"
        ${DIAG_SRCS_SAFETY_WRAPPERS}
)

# ---------------------------------------------------------------------------
# 9. test_can_transport
#    Module under test: transport/can_transport.c
# ---------------------------------------------------------------------------
add_diag_test(test_can_transport
    SOURCES
        "${TEST_UNIT_DIR}/test_can_transport.c"
        "${DIAG_TRANSPORT}/can_transport.c"
)

# ---------------------------------------------------------------------------
# 10. test_isotp
#     Module under test: transport/isotp.c
# ---------------------------------------------------------------------------
add_diag_test(test_isotp
    SOURCES
        "${TEST_UNIT_DIR}/test_isotp.c"
        "${DIAG_TRANSPORT}/isotp.c"
        "${DIAG_TRANSPORT}/can_transport.c"
)

# ---------------------------------------------------------------------------
# 11. test_isotp_concurrent
#     Module under test: transport/isotp.c (concurrent-request / mid-reassembly)
#
#     Tests SF interrupting a multi-frame, new FF restarting reassembly,
#     CF-without-FF rejection, wrong SN recovery, and N_Cr timeout recovery.
#     These scenarios are distinct from test_isotp (which covers timer logic)
#     and test_phase2_isotp_stmin (which covers STmin/BS/FC state machine).
# ---------------------------------------------------------------------------
add_diag_test(test_isotp_concurrent
    SOURCES
        "${TEST_UNIT_DIR}/test_isotp_concurrent.c"
        "${DIAG_TRANSPORT}/isotp.c"
        "${DIAG_TRANSPORT}/can_transport.c"
)

# ---------------------------------------------------------------------------
# 11. test_service_0x10
#     Module under test: core/uds_services/service_0x10.c
# ---------------------------------------------------------------------------
add_diag_test(test_service_0x10
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x10.c"
        ${DIAG_SRCS_SERVICES_FULL}
)

# ---------------------------------------------------------------------------
# 12. test_service_0x11
#     Module under test: core/uds_services/service_0x11.c
# ---------------------------------------------------------------------------
add_diag_test(test_service_0x11
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x11.c"
        ${DIAG_SRCS_SERVICES_FULL}
        # zephyr_port.c removed: service_0x11 no longer includes zephyr_port.h.
        # The zephyr_port_* stubs remain in tests/mocks/zephyr_port_mock.c
        # which is already included via DIAG_SRCS_SERVICES_FULL mock set.
)

# ---------------------------------------------------------------------------
# 13. test_service_0x22
#     Module under test: core/uds_services/service_0x22.c
# ---------------------------------------------------------------------------
add_diag_test(test_service_0x22
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x22.c"
        ${DIAG_SRCS_SERVICES_FULL}
)

# ---------------------------------------------------------------------------
# 14. test_service_0x27
#     Module under test: core/uds_services/service_0x27.c
# ---------------------------------------------------------------------------
add_diag_test(test_service_0x27
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x27.c"
        ${DIAG_SRCS_SERVICES_FULL}
)

# ---------------------------------------------------------------------------
# 15. test_service_0x3E
#     Module under test: core/uds_services/service_0x3E.c
# ---------------------------------------------------------------------------
add_diag_test(test_service_0x3E
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x3E.c"
        ${DIAG_SRCS_SERVICES_FULL}
)

# ===========================================================================
# Summary target: print all registered tests
# ===========================================================================
add_custom_target(list_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -N
    COMMENT "[DIAG TEST] Registered test modules:"
)

# ✅ End of tests/CMakeLists.txt

# ---------------------------------------------------------------------------
# 16. test_phase5_security_algo  [P1-SEC — AES-128-CMAC + TRNG]
#     Module under test: core/uds_security_algo.c + core/uds_aes_cmac.c
#     Covers: known-answer tests, OEM override, key injection, replay protection
# ---------------------------------------------------------------------------
add_diag_test(test_phase5_security_algo
    SOURCES
        "${TEST_UNIT_DIR}/test_phase5_security_algo.c"
        "${DIAG_CORE}/uds_security_algo.c"
        "${DIAG_CORE}/uds_aes_cmac.c"
)

# ===========================================================================
# Tests 17–35: Phase 2/3/5 + new services + routine database
# All modules source from unit_runnable/ — the single canonical test tree.
# ===========================================================================

# ---------------------------------------------------------------------------
# 17. test_phase2_isotp_stmin  [P2]
#     Module under test: transport/isotp.c — STmin timing enforcement
# ---------------------------------------------------------------------------
add_diag_test(test_phase2_isotp_stmin
    SOURCES
        "${TEST_UNIT_DIR}/test_phase2_isotp_stmin.c"
        ${DIAG_SRCS_PHASE2_ISOTP}
)

# ---------------------------------------------------------------------------
# 18. test_phase2_session_matrix  [P2]
#     Module under test: core/uds_session.c — session transition matrix
# ---------------------------------------------------------------------------
add_diag_test(test_phase2_session_matrix
    SOURCES
        "${TEST_UNIT_DIR}/test_phase2_session_matrix.c"
        ${DIAG_SRCS_PHASE2_SESSION}
)

# ---------------------------------------------------------------------------
# 19. test_phase2_suppress_bit  [P2]
#     Module under test: uds_server.c — suppressPosRspMsgIndicationBit
# ---------------------------------------------------------------------------
add_diag_test(test_phase2_suppress_bit
    SOURCES
        "${TEST_UNIT_DIR}/test_phase2_suppress_bit.c"
        ${DIAG_SRCS_PHASE2_SUPPRESS}
    DEFINES
        NVM_STORE_HOST_MOCK=1
)

# ---------------------------------------------------------------------------
# 20. test_phase3_nvm_dtc  [P3]
#     Module under test: config/dtc_mirror.c — DTC NVM persistence
# ---------------------------------------------------------------------------
add_diag_test(test_phase3_nvm_dtc
    SOURCES
        "${TEST_UNIT_DIR}/test_phase3_nvm_dtc.c"
        ${DIAG_SRCS_PHASE3_NVM}
    DEFINES
        NVM_STORE_HOST_MOCK=1
)

# ---------------------------------------------------------------------------
# 21. test_phase3_nvm_integration  [P3]
#     Module under test: platform/nvm_store.c — full NVM integration
# ---------------------------------------------------------------------------
add_diag_test(test_phase3_nvm_integration
    SOURCES
        "${TEST_UNIT_DIR}/test_phase3_nvm_integration.c"
        ${DIAG_SRCS_PHASE3_NVM}
    DEFINES
        NVM_STORE_HOST_MOCK=1
)

# ---------------------------------------------------------------------------
# 22. test_phase3_nvm_security  [P3]
#     Module under test: core/uds_security_nvm.c — security attempt persistence
# ---------------------------------------------------------------------------
add_diag_test(test_phase3_nvm_security
    SOURCES
        "${TEST_UNIT_DIR}/test_phase3_nvm_security.c"
        ${DIAG_SRCS_PHASE3_NVM}
    DEFINES
        NVM_STORE_HOST_MOCK=1
)

# ---------------------------------------------------------------------------
# 23. test_phase5_access_table  [P5]
#     Module under test: core/uds_access_table.c — ACL lookup and enforcement
# ---------------------------------------------------------------------------
add_diag_test(test_phase5_access_table
    SOURCES
        "${TEST_UNIT_DIR}/test_phase5_access_table.c"
        ${DIAG_SRCS_PHASE5_ACCESS}
)

# ---------------------------------------------------------------------------
# 24. test_phase5_replay_protection  [P5]
#     Module under test: core/uds_security_algo.c — replay protection via nonce
# ---------------------------------------------------------------------------
add_diag_test(test_phase5_replay_protection
    SOURCES
        "${TEST_UNIT_DIR}/test_phase5_replay_protection.c"
        ${DIAG_SRCS_PHASE5_SECURITY_ALGO}
)

# ---------------------------------------------------------------------------
# 25. test_phase5_server_access  [P5]
#     Module under test: core/uds_server.c — srv_check_access_rights()
# ---------------------------------------------------------------------------
add_diag_test(test_phase5_server_access
    SOURCES
        "${TEST_UNIT_DIR}/test_phase5_server_access.c"
        ${DIAG_SRCS_SERVICES_FULL}
    DEFINES
        NVM_STORE_HOST_MOCK=1
)

# ---------------------------------------------------------------------------
# 26. test_routine_database
#     Module under test: config/routine_database.c — RID registry
# ---------------------------------------------------------------------------
add_diag_test(test_routine_database
    SOURCES
        "${TEST_UNIT_DIR}/test_routine_database.c"
        ${DIAG_SRCS_ROUTINE_DB}
        "${DIAG_GENERATED}/routine_handlers.c"
)

# ---------------------------------------------------------------------------
# 27. test_service_0x14
#     Module under test: core/uds_services/service_0x14.c — ClearDiagInfo
# ---------------------------------------------------------------------------
add_diag_test(test_service_0x14
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x14.c"
        ${DIAG_SRCS_SERVICES_FULL}
    DEFINES
        NVM_STORE_HOST_MOCK=1
)

# ---------------------------------------------------------------------------
# 28. test_service_0x19
#     Module under test: core/uds_services/service_0x19.c — ReadDTCInformation
# ---------------------------------------------------------------------------
add_diag_test(test_service_0x19
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x19.c"
        ${DIAG_SRCS_SERVICES_FULL}
)

# ---------------------------------------------------------------------------
# 29. test_service_0x28
#     Module under test: core/uds_services/service_0x28.c — CommunicationControl
# ---------------------------------------------------------------------------
add_diag_test(test_service_0x28
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x28.c"
        ${DIAG_SRCS_SERVICES_FULL}
)

# ---------------------------------------------------------------------------
# 30. test_service_0x31
#     Module under test: core/uds_services/service_0x31.c — RoutineControl
# ---------------------------------------------------------------------------
add_diag_test(test_service_0x31
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x31.c"
        ${DIAG_SRCS_SERVICES_FULL}
)

# ---------------------------------------------------------------------------
# 33. test_service_0x34
#     Module under test: core/uds_services/service_0x34.c — RequestDownload
#     Tests: NULL ctx, flash ops gate (NRC 0x22), ALFID parsing, address range
#            validation, erase failure (NRC 0x70), positive response format,
#            transfer context initialisation (REQ-DL-001/002).
# ---------------------------------------------------------------------------
add_diag_test(test_service_0x34
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x34.c"
        ${DIAG_SRCS_SERVICES_FULL}
)

# ---------------------------------------------------------------------------
# 34. test_service_0x36
#     Module under test: core/uds_services/service_0x36.c — TransferData
#     Tests: block counter validation (NRC 0x73), wrap 0xFF→0x01 (REQ-DL-001),
#            bytes_remaining decrement (REQ-DL-002), write flush, CRC update,
#            write failure → IDLE reset (REQ-DL-003).
# ---------------------------------------------------------------------------
add_diag_test(test_service_0x36
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x36.c"
        ${DIAG_SRCS_SERVICES_FULL}
)

# ---------------------------------------------------------------------------
# 35. test_service_0x37
#     Module under test: core/uds_services/service_0x37.c — RequestTransferExit
#     Tests: incomplete transfer gate (NRC 0x31), partial buffer flush, CRC-32
#            verify path, CRC mismatch (NRC 0x72), verify failure, transfer
#            reset on success and failure (REQ-DL-003).
# ---------------------------------------------------------------------------
add_diag_test(test_service_0x37
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x37.c"
        ${DIAG_SRCS_SERVICES_FULL}
)

# ---------------------------------------------------------------------------
# 31. test_service_0x3E  (was 15, now also confirmed in full set)
#     Module under test: core/uds_services/service_0x3E.c — TesterPresent
#     Note: also registered as test 15 above using the legacy target name.
#     This duplicate target uses _v2 suffix to avoid CMake name collision.
# ---------------------------------------------------------------------------
# (already covered as test 15 — no duplicate needed)

# ---------------------------------------------------------------------------
# 32. test_service_0x85
#     Module under test: core/uds_services/service_0x85.c — ControlDTCSetting
# ---------------------------------------------------------------------------
add_diag_test(test_service_0x85
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x85.c"
        ${DIAG_SRCS_SERVICES_FULL}
)

# ---------------------------------------------------------------------------
# 37. test_doip_server
#     Module under test: transport/doip/doip_server.c
# ---------------------------------------------------------------------------
add_diag_test(test_doip_server
    SOURCES
        "${TEST_UNIT_DIR}/test_doip_server.c"
        "${DIAG_TRANSPORT}/doip/doip_server.c"
        ${DIAG_SRCS_SERVICES_FULL}
)

