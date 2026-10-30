# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Xaloqi
#
# EDS#543: per-block readback during TransferData. Runs real 0x34 -> 0x36 ->
# 0x37 downloads through platform/zephyr/zephyr_flash_ops.c into the image_1
# partition of native_sim's flash simulator, with one flash write corrupted on
# purpose, and checks that the block is refused at its 0x36 and that 0x37 does
# not read the region back again.
#
#   west build -b native_sim EDS/tests/zephyr_flash_verify && build/zephyr/zephyr.exe
cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/eds_zephyr_module.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eds_zephyr_flash_verify)

set(DIAG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

target_include_directories(app PRIVATE
    ${DIAG_ROOT}/core
    ${DIAG_ROOT}/core/uds_services
    ${DIAG_ROOT}/transport
    ${DIAG_ROOT}/config
    ${DIAG_ROOT}/platform
    ${DIAG_ROOT}/platform/zephyr
)
target_sources(app PRIVATE
    src/main.c
    ${DIAG_ROOT}/core/uds_services/service_0x34.c
    ${DIAG_ROOT}/core/uds_services/service_0x36.c
    ${DIAG_ROOT}/core/uds_services/service_0x37.c
    ${DIAG_ROOT}/core/uds_services/service_registration.c
    ${DIAG_ROOT}/core/uds_transfer_ctx.c
    ${DIAG_ROOT}/platform/uds_flash_ops.c
    ${DIAG_ROOT}/platform/uds_image_policy.c
    ${DIAG_ROOT}/platform/uds_mcuboot_image.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_flash_ops.c
)
target_compile_definitions(app PRIVATE EDS_MSG_BUF_MAX_STACK_BYTES=8192)
