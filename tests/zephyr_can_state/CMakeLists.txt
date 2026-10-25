# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Xaloqi
#
# EDS#513: platform/zephyr/zephyr_can.c reports bus-off through Zephyr's CAN
# state-change callback (single callback on Zephyr <= 4.4, a callback list on
# >= 4.5). This test drives a real state change through the in-tree
# can_loopback driver and checks that the handler is registered and sets /
# clears the bus-off flag, on whichever Zephyr line it is built against.
#
#   west build -b native_sim EDS/tests/zephyr_can_state -- \
#       -DEXTRA_CONF_FILE=... (none needed) && build/zephyr/zephyr.exe
cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/eds_zephyr_module.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eds_zephyr_can_state)

set(DIAG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

target_include_directories(app PRIVATE
    ${DIAG_ROOT}/core
    ${DIAG_ROOT}/transport
    ${DIAG_ROOT}/config
    ${DIAG_ROOT}/platform
    ${DIAG_ROOT}/platform/zephyr
)
target_sources(app PRIVATE
    src/main.c
    ${DIAG_ROOT}/transport/can_transport.c
    ${DIAG_ROOT}/platform/zephyr/zephyr_can.c
)
# uds_msg_buf_t is large; the examples raise this limit on native_sim for the same
# reason (a host process stack is big). Nothing here puts one on the stack.
target_compile_definitions(app PRIVATE EDS_MSG_BUF_MAX_STACK_BYTES=8192)
