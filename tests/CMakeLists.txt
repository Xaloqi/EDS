# =============================================================================
# Xaloqi EDS
# FILE: tests/CMakeLists.txt
#
# PURPOSE: Host-native CMake build for all 43 unit test modules.
#          Compiles each test_*.c file together with the FULL diagnostics
#          stack (mirroring build_tests.sh's STACK_SRCS) and the Unity
#          framework into a standalone executable. CTest then runs each
#          executable and reports pass/fail based on exit code.
#
#          Architecture:
#            tests/CMakeLists.txt (this file)
#              -> tests/runner/test_main.c         (Unity main + lifecycle)
#              -> tests/runner/ztest_shim.h        (ZTEST-to-Unity macro map)
#              -> project/unity/unity.c            (Unity assertion engine)
#              -> tests/unit_runnable/test_<module>.c  (canonical test cases — 43 modules)
#              -> <production sources>             (the entire diagnostics stack)
#              -> examples/basic_ecu/generated/ did_handlers.c etc. (generated stubs)
#
#          This design allows all 43 modules to be tested on a CI runner
#          without a Zephyr SDK or hardware target, using only GCC/Clang
#          and CMake. Each test module compiles independently to avoid
#          link-time symbol conflicts between test modules.
#
#          [EDS#88 FIX] This file previously (a) pointed generated sources at
#          <repo-root>/generated/, which does not exist — the real location
#          is examples/basic_ecu/generated/, matching build_tests.sh; (b) did
#          not define the compile flags build_tests.sh's CFLAGS array passes
#          (UNIT_TEST, NVM_STORE_HOST_MOCK, ISOTP_ENABLE_CAN_FD,
#          ISOTP_TX_PADDING, EDS_MSG_BUF_MAX_STACK_BYTES), so every target
#          failed core/uds_types.h's _Static_assert; and (c) gave each
#          add_diag_test() call its own hand-maintained, drifted SOURCES list
#          instead of linking the whole stack the way build_tests.sh does,
#          so targets failed to link with undefined-reference errors. Fixed
#          by introducing DIAG_STACK_SRCS below, which mirrors
#          build_tests.sh's STACK_SRCS array file-for-file.
#
# USAGE:
#   # Build and run all tests:
#   cmake -S tests -B build
#   cmake --build build -j4
#   ctest --test-dir build --output-on-failure
#
#   # Run a single test module:
#   ./build/test_uds_session
#
#   # Run with verbose output:
#   ctest --test-dir build -V
#
# CI INTEGRATION:
#   ctest returns 0 on all pass, non-zero on any failure.
#   Use --output-on-failure to see test output only when something fails.
#   Use --parallel <N> to run test executables concurrently.
#
# SAFETY CONTEXT : Test infrastructure — not safety-assessed.
# CODING STANDARD: MISRA C:2012 alignment intended (advisory for test code).
# VERSION        : 1.8.0 (43 modules wired — parity with build_tests.sh; see EDS#88)
# SPDX-License-Identifier: Apache-2.0
# =============================================================================

cmake_minimum_required(VERSION 3.16)

project(diag_unit_tests
    VERSION     1.8.0
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
set(DIAG_TRANSPORT_DOIP "${DIAG_ROOT}/transport/doip")
set(DIAG_CONFIG        "${DIAG_ROOT}/config")
set(DIAG_PLATFORM      "${DIAG_ROOT}/platform")
set(DIAG_PLATFORM_ZEPHYR "${DIAG_ROOT}/platform/zephyr")
# [EDS#88] Generated sources live under the example that owns them, not at
# <repo-root>/generated/ (which does not exist — see .gitignore comment "Root
# generated/ removed — each example owns its own generated/ subfolder").
# This matches build_tests.sh's STACK_SRCS / INCLUDES exactly.
set(DIAG_GENERATED     "${DIAG_ROOT}/examples/basic_ecu/generated")
set(DIAG_UNITY         "${DIAG_ROOT}/project/unity")
# [M2] Canonical test directory is unit_runnable/ (43 modules).
# tests/legacy_unit/ has been removed — unit_runnable/ is the single source of truth.
# build_tests.sh and this CMakeLists.txt both reference unit_runnable/.
set(TEST_UNIT_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/unit_runnable")
set(TEST_RUNNER_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/runner")
set(TEST_MOCKS_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/mocks")

# ---------------------------------------------------------------------------
# Global compile definitions [EDS#88 — Level 2 fix]
#
# Mirrors build_tests.sh's CFLAGS array exactly. Applied to every target in
# this directory (the unity library and all add_diag_test() executables),
# the same way build_tests.sh applies its CFLAGS to every gcc invocation.
#
#   UNIT_TEST=1                  activates test-only resets/hooks
#   NVM_STORE_HOST_MOCK=1        substitutes a malloc-backed NVM store for
#                                 the Zephyr flash driver
#   ISOTP_ENABLE_CAN_FD=1        enables CAN FD ISO-TP paths (test_isotp_canfd)
#   ISOTP_TX_PADDING=1           enables TX padding code path
#   EDS_MSG_BUF_MAX_STACK_BYTES=8192
#                                 suppresses the uds_msg_buf_t stack-size
#                                 _Static_assert in core/uds_types.h on host
#                                 builds (host has an 8 MB process stack; the
#                                 assert exists to guard embedded targets,
#                                 which default to 256 bytes)
#
# ZTEST_HOST_SHIM=1 is defined below via DIAG_TEST_COMPILE_OPTIONS (it was
# already correct and applies only to test executables, not the unity lib).
# ---------------------------------------------------------------------------
add_compile_definitions(
    UNIT_TEST=1
    NVM_STORE_HOST_MOCK=1
    ISOTP_ENABLE_CAN_FD=1
    ISOTP_TX_PADDING=1
    EDS_MSG_BUF_MAX_STACK_BYTES=8192
)

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
#
# Mirrors build_tests.sh's INCLUDES array (adds transport/doip and
# tests/mocks, which the previous version of this file omitted).
# ---------------------------------------------------------------------------
set(DIAG_INCLUDE_DIRS
    "${DIAG_CORE}"
    "${DIAG_SERVICES}"
    "${DIAG_TRANSPORT}"
    "${DIAG_TRANSPORT_DOIP}"
    "${DIAG_CONFIG}"
    "${DIAG_PLATFORM}"
    "${DIAG_PLATFORM_ZEPHYR}"
    "${DIAG_GENERATED}"
    "${DIAG_UNITY}"
    "${TEST_RUNNER_DIR}"
    "${TEST_MOCKS_DIR}"
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
# Helper function: add_diag_test(name SOURCES sources... [DEFINES ...])
#
# Creates a test executable named 'name' that:
#   - Compiles the listed source files, plus tests/runner/test_main.c
#   - Links against Unity
#   - Includes all DIAG_INCLUDE_DIRS
#   - Applies DIAG_TEST_COMPILE_OPTIONS
#   - Registers with CTest via add_test()
#
# NOTE: test_main.c is added here unconditionally — do not also list it in
# a call's SOURCES (CMake errors on a source file listed twice in one
# add_executable). Likewise unity.c is provided via the 'unity' link
# library, not as a direct source, to avoid linking its symbols twice.
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
# DIAG_STACK_SRCS [EDS#88 — Level 3 fix]
#
# The FULL diagnostics stack, linked into EVERY test executable — exactly
# mirroring build_tests.sh's STACK_SRCS array (same files, same order,
# 1:1). Every test module exercises integration between modules, not just
# the module under test in isolation. This is what makes the individual
# per-target hand-maintained SOURCES lists (previously the pattern in this
# file) unnecessary and prone to drift: one dependency graph, one list.
#
# Two files from STACK_SRCS's bash array are deliberately NOT repeated here:
#   - project/unity/unity.c   — provided via the 'unity' static library
#                                (target_link_libraries), not as a direct
#                                source; listing it again would double-link
#                                its symbols.
#   - tests/runner/test_main.c — added unconditionally inside add_diag_test()
#                                 above; listing it again is a duplicate
#                                 source file, which CMake rejects.
# ===========================================================================
set(DIAG_STACK_SRCS
    # UDS core
    "${DIAG_CORE}/uds_server.c"
    "${DIAG_CORE}/uds_session.c"
    "${DIAG_CORE}/uds_session_stats.c"
    "${DIAG_CORE}/uds_security.c"
    "${DIAG_CORE}/uds_aes_cmac.c"          # [P1-SEC] AES-128-CMAC primitive
    "${DIAG_CORE}/uds_security_algo.c"
    "${DIAG_CORE}/uds_security_nvm.c"
    "${DIAG_CORE}/uds_safety.c"
    "${DIAG_CORE}/uds_access_table.c"
    "${DIAG_CORE}/uds_comm_control.c"
    # Service handlers
    "${DIAG_SERVICES}/service_registration.c"
    "${DIAG_SERVICES}/service_0x10.c"
    "${DIAG_SERVICES}/service_0x11.c"
    "${DIAG_SERVICES}/service_0x14.c"
    "${DIAG_SERVICES}/service_0x19.c"
    "${DIAG_SERVICES}/service_0x22.c"
    "${DIAG_SERVICES}/service_0x27.c"
    "${DIAG_SERVICES}/service_0x28.c"
    "${DIAG_SERVICES}/service_0x2A.c"   # [0x2A] ReadDataByPeriodicIdentifier
    "${DIAG_CORE}/uds_periodic.c"       # [0x2A] periodic scheduler
    "${DIAG_SERVICES}/service_0x2E.c"
    "${DIAG_SERVICES}/service_0x2F.c"   # InputOutputControlByIdentifier
    "${DIAG_SERVICES}/service_0x31.c"
    "${DIAG_SERVICES}/service_0x3E.c"
    "${DIAG_SERVICES}/service_0x85.c"
    # DFU services — service_registration.c references all three handlers
    # in g_uds_service_table[]; omitting any of these causes every test
    # binary to fail at link with undefined reference errors.
    "${DIAG_SERVICES}/service_0x23.c"   # ReadMemoryByAddress
    "${DIAG_SERVICES}/service_0x34.c"   # RequestDownload
    "${DIAG_SERVICES}/service_0x35.c"   # RequestUpload
    "${DIAG_SERVICES}/service_0x36.c"   # TransferData
    "${DIAG_SERVICES}/service_0x37.c"   # RequestTransferExit
    "${DIAG_SERVICES}/service_0x3D.c"   # WriteMemoryByAddress
    "${DIAG_CORE}/uds_transfer_ctx.c"   # DFU transfer state machine
    "${DIAG_PLATFORM}/uds_flash_ops.c"  # Flash ops singleton
    # Transport
    "${DIAG_TRANSPORT}/isotp.c"
    "${DIAG_TRANSPORT}/can_transport.c"
    # DoIP transport (core logic only, no platform/network deps)
    "${DIAG_TRANSPORT_DOIP}/doip_server.c"
    # Config databases
    "${DIAG_CONFIG}/did_database.c"
    "${DIAG_CONFIG}/dtc_database.c"
    "${DIAG_CONFIG}/dtc_mirror.c"
    "${DIAG_CONFIG}/routine_database.c"
    # Generated sources (pre-committed under examples/basic_ecu/generated/;
    # run tools/codegen.py to regenerate — see build_tests.sh header)
    "${DIAG_GENERATED}/did_handlers.c"
    "${DIAG_GENERATED}/did_safety_wrappers.c"
    "${DIAG_GENERATED}/routine_handlers.c"
    # Platform: host mock replaces Zephyr flash driver
    "${DIAG_PLATFORM_ZEPHYR}/nvm_store_mock.c"
    # Host mocks for Zephyr kernel APIs
    "${TEST_MOCKS_DIR}/zephyr_port_mock.c"
)

# ===========================================================================
# Test targets — one per module under test (43 total, matching
# build_tests.sh's TESTS array 1:1 — see EDS#88)
# ===========================================================================

add_diag_test(test_uds_session
    SOURCES
        "${TEST_UNIT_DIR}/test_uds_session.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_uds_security
    SOURCES
        "${TEST_UNIT_DIR}/test_uds_security.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_uds_safety
    SOURCES
        "${TEST_UNIT_DIR}/test_uds_safety.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_uds_server
    SOURCES
        "${TEST_UNIT_DIR}/test_uds_server.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_did_database
    SOURCES
        "${TEST_UNIT_DIR}/test_did_database.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_dtc_database
    SOURCES
        "${TEST_UNIT_DIR}/test_dtc_database.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_dtc_mirror
    SOURCES
        "${TEST_UNIT_DIR}/test_dtc_mirror.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_routine_database
    SOURCES
        "${TEST_UNIT_DIR}/test_routine_database.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_did_handlers
    SOURCES
        "${TEST_UNIT_DIR}/test_did_handlers.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_did_safety_wrappers
    SOURCES
        "${TEST_UNIT_DIR}/test_did_safety_wrappers.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_can_transport
    SOURCES
        "${TEST_UNIT_DIR}/test_can_transport.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_isotp
    SOURCES
        "${TEST_UNIT_DIR}/test_isotp.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_isotp_concurrent
    SOURCES
        "${TEST_UNIT_DIR}/test_isotp_concurrent.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x10
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x10.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x11
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x11.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x14
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x14.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x19
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x19.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x22
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x22.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x27
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x27.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x28
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x28.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_uds_periodic
    SOURCES
        "${TEST_UNIT_DIR}/test_uds_periodic.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x2A
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x2A.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x2F
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x2F.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x31
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x31.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x23_0x3D
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x23_0x3D.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x34
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x34.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x35
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x35.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x36
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x36.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x37
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x37.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x3E
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x3E.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_service_0x85
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x85.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_phase2_suppress_bit
    SOURCES
        "${TEST_UNIT_DIR}/test_phase2_suppress_bit.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_phase2_session_matrix
    SOURCES
        "${TEST_UNIT_DIR}/test_phase2_session_matrix.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_phase2_isotp_stmin
    SOURCES
        "${TEST_UNIT_DIR}/test_phase2_isotp_stmin.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_phase3_nvm_security
    SOURCES
        "${TEST_UNIT_DIR}/test_phase3_nvm_security.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_phase3_nvm_dtc
    SOURCES
        "${TEST_UNIT_DIR}/test_phase3_nvm_dtc.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_phase3_nvm_integration
    SOURCES
        "${TEST_UNIT_DIR}/test_phase3_nvm_integration.c"
        ${DIAG_STACK_SRCS}
)

# ---------------------------------------------------------------------------
# test_phase5_security_algo  [P1-SEC — AES-128-CMAC + TRNG]
#     Module under test: core/uds_security_algo.c + core/uds_aes_cmac.c
#     Covers: known-answer tests, OEM override, key injection, replay
#     protection. uds_safety.c is required (already part of DIAG_STACK_SRCS):
#     uds_security_algo.c calls uds_safety_record_platform_violation() on
#     TRNG fault, and the dev-mode TRNG regression tests assert on the
#     safety context.
# ---------------------------------------------------------------------------
add_diag_test(test_phase5_security_algo
    SOURCES
        "${TEST_UNIT_DIR}/test_phase5_security_algo.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_phase5_access_table
    SOURCES
        "${TEST_UNIT_DIR}/test_phase5_access_table.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_phase5_server_access
    SOURCES
        "${TEST_UNIT_DIR}/test_phase5_server_access.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_phase5_replay_protection
    SOURCES
        "${TEST_UNIT_DIR}/test_phase5_replay_protection.c"
        ${DIAG_STACK_SRCS}
)

add_diag_test(test_doip_server
    SOURCES
        "${TEST_UNIT_DIR}/test_doip_server.c"
        ${DIAG_STACK_SRCS}
)

# ---------------------------------------------------------------------------
# test_trng_fail_closed  [SEC-TRNG-FAILCLOSED-01]
#      Module under test: core/uds_security_algo.c in its PRODUCTION
#      configuration, plus core/uds_security.c end-to-end.
#      Compiled with CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 so the entropy gate
#      selects fail-closed; that cannot be reached from the default host
#      build used by every other module here. Mirrors build_tests.sh's
#      extra_flags_for_test().
# ---------------------------------------------------------------------------
add_diag_test(test_trng_fail_closed
    SOURCES
        "${TEST_UNIT_DIR}/test_trng_fail_closed.c"
        ${DIAG_STACK_SRCS}
    DEFINES
        CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0
)

# ===========================================================================
# Summary target: print all registered tests
# ===========================================================================
add_custom_target(list_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -N
    COMMENT "[DIAG TEST] Registered test modules:"
)

# End of tests/CMakeLists.txt
