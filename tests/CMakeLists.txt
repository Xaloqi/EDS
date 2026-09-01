# =============================================================================
# Xaloqi EDS
# FILE: tests/CMakeLists.txt
#
# PURPOSE: Host-native CMake build for every unit test module in
#          tests/unit_runnable/. The FULL diagnostics stack (mirroring
#          build_tests.sh's STACK_SRCS) is linked into every test executable
#          alongside the Unity framework. CTest then runs each executable and
#          reports pass/fail based on exit code.
#
#          Architecture:
#            tests/CMakeLists.txt (this file)
#              -> tests/runner/test_main.c         (Unity main + lifecycle)
#              -> tests/runner/ztest_shim.h        (ZTEST-to-Unity macro map)
#              -> project/unity/unity.c            (Unity assertion engine)
#              -> tests/unit_runnable/test_<module>.c  (canonical test cases)
#              -> <production sources>             (the entire diagnostics stack)
#              -> examples/basic_ecu/generated/ did_handlers.c etc. (generated stubs)
#
#          This design allows every module to be tested on a CI runner
#          without a Zephyr SDK or hardware target, using only GCC/Clang
#          and CMake.
#
#          [#151] BUILD ONCE, LINK N TIMES
#          The stack used to be listed in every single add_diag_test() call,
#          so CMake compiled all ~44 stack sources once per test target
#          (~1,900 translation units). It is now compiled once into an
#          OBJECT library whose objects are linked into every test
#          executable; only each module's own driver .c is compiled
#          per-target. This mirrors build_tests.sh's libeds_testable.a.
#
#          Why an OBJECT library and not a STATIC one: every object must end
#          up in every test binary, exactly as when the sources were listed
#          per-target. A plain static-archive link would extract only the
#          members needed to resolve an already-undefined symbol and would
#          silently drop the rest (measured on this codebase: 279 symbols,
#          including g_uds_service_table and the whole DID handler set, with
#          the affected module still reporting PASS). build_tests.sh gets
#          that guarantee from GNU ld's --whole-archive; the portable CMake
#          equivalent, on the 3.16 baseline this file targets, is an OBJECT
#          library. Same semantics, expressed in each build system's idiom —
#          what O-26 requires the two paths to keep in lockstep is the source
#          list and the compile definitions, and those remain mirrored 1:1.
#
#          Two modules need the SHARED sources compiled in a different
#          configuration and therefore get their own OBJECT library variant;
#          see eds_stack_objects() and build_tests.sh's extra_flags_for_test().
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
# VERSION        : 1.9.0 (shared stack OBJECT library — see #151; module list
#                  stays at parity with build_tests.sh's TESTS array, see EDS#88)
# SPDX-License-Identifier: Apache-2.0
# =============================================================================

cmake_minimum_required(VERSION 3.16)

project(diag_unit_tests
    VERSION     1.9.0
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
# [M2] Canonical test directory is unit_runnable/.
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
# [#151] eds_stack_objects(OUT_VAR [<defines>...])
#
# Returns (in OUT_VAR) the name of the OBJECT library holding the whole
# diagnostics stack compiled with the given extra compile definitions,
# creating it on first use. One library per distinct definition set:
#
#   no defines                            -> eds_stack_default
#   CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0   -> eds_stack_config_diag_placeholder_keys_only_0
#   UDS_ACL_ALLOW_UNLISTED_SERVICES=1     -> eds_stack_uds_acl_allow_unlisted_services_1
#
# The library name is DERIVED FROM THE DEFINES, exactly as build_tests.sh
# derives its archive variant id from extra_flags_for_test()'s output. That
# is the point: a module that needs a third compile-time configuration of the
# shared sources automatically gets a third library, instead of silently
# linking the default one and "proving" a configuration it never compiled.
#
# test_main.c is part of the stack here (it is identical for every module and
# carries the weak setUp/tearDown defaults each test_*.c overrides), matching
# build_tests.sh's STACK_SRCS. unity.c is NOT: it is provided by the 'unity'
# static library below, which is compiled once and shared by every target.
# ---------------------------------------------------------------------------
function(eds_stack_objects OUT_VAR)
    set(_defs ${ARGN})

    if(NOT _defs)
        set(_objlib "eds_stack_default")
    else()
        string(REPLACE ";" "_" _slug "${_defs}")
        string(REGEX REPLACE "[^A-Za-z0-9]+" "_" _slug "${_slug}")
        string(REGEX REPLACE "^_+|_+$" "" _slug "${_slug}")
        string(TOLOWER "${_slug}" _slug)
        set(_objlib "eds_stack_${_slug}")
    endif()

    if(NOT TARGET ${_objlib})
        add_library(${_objlib} OBJECT
            "${TEST_RUNNER_DIR}/test_main.c"
            ${DIAG_STACK_SRCS}
        )
        target_include_directories(${_objlib} PRIVATE ${DIAG_INCLUDE_DIRS})
        target_compile_options(${_objlib} PRIVATE ${DIAG_TEST_COMPILE_OPTIONS})
        if(_defs)
            target_compile_definitions(${_objlib} PRIVATE ${_defs})
        endif()
    endif()

    set(${OUT_VAR} ${_objlib} PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Helper function: add_diag_test(name SOURCES sources... [DEFINES ...])
#
# Creates a test executable named 'name' that:
#   - Compiles the listed source files (just the module's own driver .c)
#   - Links in every object of the shared stack OBJECT library built with
#     this target's DEFINES  [#151]
#   - Links against Unity
#   - Includes all DIAG_INCLUDE_DIRS
#   - Applies DIAG_TEST_COMPILE_OPTIONS
#   - Registers with CTest via add_test()
#
# NOTE: neither test_main.c nor unity.c may be listed in a call's SOURCES.
# test_main.c comes from the stack OBJECT library and unity.c from the
# 'unity' link library; listing either again would duplicate its symbols.
# ---------------------------------------------------------------------------
function(add_diag_test TEST_NAME)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEFINES" ${ARGN})

    eds_stack_objects(_stack_objlib ${ARG_DEFINES})

    add_executable(${TEST_NAME}
        ${ARG_SOURCES}
        $<TARGET_OBJECTS:${_stack_objlib}>
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
# [#151] This list is now compiled ONCE per configuration, into the OBJECT
# library that eds_stack_objects() creates, rather than once per test target.
#
# Two files from STACK_SRCS's bash array are deliberately NOT repeated here:
#   - project/unity/unity.c   — provided via the 'unity' static library
#                                (target_link_libraries), not as a direct
#                                source; listing it again would double-link
#                                its symbols.
#   - tests/runner/test_main.c — added inside eds_stack_objects() above, so
#                                 it is compiled once with the stack rather
#                                 than once per test target.
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
# Test targets — one per module under test, matching build_tests.sh's
# TESTS array 1:1 (see EDS#88). The count is deliberately not written down
# here: ci.yml's "Verify test count" step and scripts/verify_doc_counts.py
# are what keep it honest, and a number in a comment has drifted twice.
# ===========================================================================

add_diag_test(test_uds_session
    SOURCES
        "${TEST_UNIT_DIR}/test_uds_session.c"
)

add_diag_test(test_uds_security
    SOURCES
        "${TEST_UNIT_DIR}/test_uds_security.c"
)

add_diag_test(test_uds_safety
    SOURCES
        "${TEST_UNIT_DIR}/test_uds_safety.c"
)

add_diag_test(test_uds_server
    SOURCES
        "${TEST_UNIT_DIR}/test_uds_server.c"
)

add_diag_test(test_did_database
    SOURCES
        "${TEST_UNIT_DIR}/test_did_database.c"
)

add_diag_test(test_dtc_database
    SOURCES
        "${TEST_UNIT_DIR}/test_dtc_database.c"
)

add_diag_test(test_dtc_mirror
    SOURCES
        "${TEST_UNIT_DIR}/test_dtc_mirror.c"
)

add_diag_test(test_routine_database
    SOURCES
        "${TEST_UNIT_DIR}/test_routine_database.c"
)

add_diag_test(test_did_handlers
    SOURCES
        "${TEST_UNIT_DIR}/test_did_handlers.c"
)

add_diag_test(test_did_safety_wrappers
    SOURCES
        "${TEST_UNIT_DIR}/test_did_safety_wrappers.c"
)

add_diag_test(test_can_transport
    SOURCES
        "${TEST_UNIT_DIR}/test_can_transport.c"
)

add_diag_test(test_isotp
    SOURCES
        "${TEST_UNIT_DIR}/test_isotp.c"
)

add_diag_test(test_isotp_concurrent
    SOURCES
        "${TEST_UNIT_DIR}/test_isotp_concurrent.c"
)

add_diag_test(test_service_0x10
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x10.c"
)

add_diag_test(test_service_0x11
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x11.c"
)

add_diag_test(test_service_0x14
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x14.c"
)

add_diag_test(test_service_0x19
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x19.c"
)

add_diag_test(test_service_0x22
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x22.c"
)

add_diag_test(test_service_0x27
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x27.c"
)

add_diag_test(test_service_0x28
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x28.c"
)

add_diag_test(test_service_0x2E
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x2E.c"
)

add_diag_test(test_uds_periodic
    SOURCES
        "${TEST_UNIT_DIR}/test_uds_periodic.c"
)

add_diag_test(test_service_0x2A
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x2A.c"
)

add_diag_test(test_service_0x2F
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x2F.c"
)

add_diag_test(test_service_0x31
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x31.c"
)

add_diag_test(test_service_0x23_0x3D
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x23_0x3D.c"
)

add_diag_test(test_service_0x34
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x34.c"
)

add_diag_test(test_service_0x35
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x35.c"
)

add_diag_test(test_service_0x36
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x36.c"
)

add_diag_test(test_service_0x37
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x37.c"
)

add_diag_test(test_service_0x3E
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x3E.c"
)

add_diag_test(test_service_0x85
    SOURCES
        "${TEST_UNIT_DIR}/test_service_0x85.c"
)

add_diag_test(test_phase2_suppress_bit
    SOURCES
        "${TEST_UNIT_DIR}/test_phase2_suppress_bit.c"
)

add_diag_test(test_phase2_session_matrix
    SOURCES
        "${TEST_UNIT_DIR}/test_phase2_session_matrix.c"
)

add_diag_test(test_phase2_isotp_stmin
    SOURCES
        "${TEST_UNIT_DIR}/test_phase2_isotp_stmin.c"
)

add_diag_test(test_phase3_nvm_security
    SOURCES
        "${TEST_UNIT_DIR}/test_phase3_nvm_security.c"
)

add_diag_test(test_phase3_nvm_dtc
    SOURCES
        "${TEST_UNIT_DIR}/test_phase3_nvm_dtc.c"
)

add_diag_test(test_phase3_nvm_integration
    SOURCES
        "${TEST_UNIT_DIR}/test_phase3_nvm_integration.c"
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
)

add_diag_test(test_phase5_access_table
    SOURCES
        "${TEST_UNIT_DIR}/test_phase5_access_table.c"
)

add_diag_test(test_phase5_server_access
    SOURCES
        "${TEST_UNIT_DIR}/test_phase5_server_access.c"
)

add_diag_test(test_phase5_replay_protection
    SOURCES
        "${TEST_UNIT_DIR}/test_phase5_replay_protection.c"
)

add_diag_test(test_doip_server
    SOURCES
        "${TEST_UNIT_DIR}/test_doip_server.c"
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
    DEFINES
        CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0
)

# ---------------------------------------------------------------------------
# test_uds_acl_permissive_opt_in  [#113]
#      Module under test: core/uds_access_table.c / core/uds_server.c in
#      the UDS_ACL_ALLOW_UNLISTED_SERVICES=1 (permissive opt-in) build
#      configuration, which cannot be reached from the default host build
#      used by every other test module here. Mirrors test_trng_fail_closed's
#      per-target DEFINES pattern.
# ---------------------------------------------------------------------------
add_diag_test(test_uds_acl_permissive_opt_in
    SOURCES
        "${TEST_UNIT_DIR}/test_uds_acl_permissive_opt_in.c"
    DEFINES
        UDS_ACL_ALLOW_UNLISTED_SERVICES=1
)

# ===========================================================================
# Summary target: print all registered tests
# ===========================================================================
add_custom_target(list_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -N
    COMMENT "[DIAG TEST] Registered test modules:"
)

# End of tests/CMakeLists.txt
