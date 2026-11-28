# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Xaloqi
#
# EDS#56: the SecurityAccess key source (CONFIG_EDS_KEYS_OTP) against Zephyr's
# OTP emulator on native_sim, and against the FRDM-MCXN947's real nxp,otpc
# driver as a build. Zephyr 4.4 or newer (the OTP API): CI runs it on v4.5.

cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/eds_zephyr_module.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eds_zephyr_key_source)

set(DIAG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

target_include_directories(app PRIVATE
    ${DIAG_ROOT}/core
    ${DIAG_ROOT}/config
    ${DIAG_ROOT}/platform
    ${DIAG_ROOT}/platform/zephyr
)
target_sources(app PRIVATE
    src/main.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_key_source.c
    ${DIAG_ROOT}/core/uds_security_algo.c
    ${DIAG_ROOT}/core/uds_aes_cmac.c
)
# EDS_TEST_HOOKS exposes uds_security_algo_derive_key(), which the test compares
# against AES-CMAC under the emulator's key.
target_compile_definitions(app PRIVATE EDS_MSG_BUF_MAX_STACK_BYTES=8192 EDS_TEST_HOOKS=1)
