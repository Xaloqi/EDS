# =============================================================================
# cmake/toolchain/arm-none-eabi.cmake
#
# CMake toolchain file for ARM bare-metal cross-compilation.
# Targets: ARM Cortex-M0/M3/M4/M7 (FreeRTOS, no OS, no stdlib).
#
# Tested with: gcc-arm-none-eabi from Ubuntu 22.04 apt (arm-none-eabi-gcc 10.3.1)
#
# USAGE:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/arm-none-eabi.cmake ...
#
# This file MUST be processed before project() is called — pass it via
# -DCMAKE_TOOLCHAIN_FILE, not from inside CMakeLists.txt after project().
#
# KEY FIX — two-part solution for bare-metal cross-compilation on Ubuntu:
#
# 1. CMAKE_TRY_COMPILE_TARGET_TYPE = STATIC_LIBRARY
#    Skips the link step in CMake's compiler probe. Bare-metal targets have
#    no crt0.o or libc in the default link path — the probe always fails
#    without this setting.
#
# 2. CMAKE_SYSROOT = /usr/lib/arm-none-eabi
#    The Ubuntu gcc-arm-none-eabi package installs newlib headers at
#    /usr/lib/arm-none-eabi/include/ — NOT inside the compiler's own
#    prefix. Without --sysroot pointing here, the compiler cannot find
#    string.h, stdint.h, stddef.h, etc. at compile time.
#    CMAKE_SYSROOT tells CMake to pass --sysroot=<path> to every
#    compile and link invocation automatically.
# =============================================================================

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# ── Toolchain binaries ──────────────────────────────────────────────────────

find_program(ARM_GCC_FOUND
    NAMES arm-none-eabi-gcc arm-zephyr-eabi-gcc
    DOC   "ARM bare-metal C compiler"
)

if(NOT ARM_GCC_FOUND)
    message(FATAL_ERROR
        "No ARM cross-compiler found. Install one of:\n"
        "  apt-get install gcc-arm-none-eabi\n"
        "  (or install the Zephyr SDK and set PATH accordingly)")
endif()

get_filename_component(TOOLCHAIN_BIN_DIR  "${ARM_GCC_FOUND}" DIRECTORY)
get_filename_component(TOOLCHAIN_COMPILER "${ARM_GCC_FOUND}" NAME)
string(REPLACE "-gcc" "" TOOLCHAIN_PREFIX "${TOOLCHAIN_COMPILER}")

set(CMAKE_C_COMPILER   "${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}-gcc")
set(CMAKE_ASM_COMPILER "${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}-gcc")
set(CMAKE_AR           "${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}-ar")
set(CMAKE_RANLIB       "${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}-ranlib")
set(CMAKE_OBJCOPY      "${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}-objcopy"
    CACHE FILEPATH "objcopy")
set(CMAKE_SIZE         "${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}-size"
    CACHE FILEPATH "size")

# ── Skip CMake's link-test (bare-metal has no crt0.o / libc in default path) ─
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ── Sysroot: points the compiler at its newlib header installation ───────────
#
# Ubuntu's gcc-arm-none-eabi package installs newlib (string.h, stdio.h, etc.)
# under /usr/lib/arm-none-eabi/, NOT inside the gcc prefix. Without this,
# every file that includes <string.h> fails with:
#   fatal error: string.h: No such file or directory
#
# CMAKE_SYSROOT causes CMake to pass --sysroot=<path> to both compile and
# link commands. The compiler then searches:
#   <sysroot>/include/          → newlib headers (string.h, etc.)
#   <sysroot>/lib/              → newlib libraries (libc.a, etc.)
#
# For the Zephyr SDK toolchain (arm-zephyr-eabi-gcc), the sysroot lives
# inside the SDK at <SDK>/arm-zephyr-eabi/arm-zephyr-eabi/. If using the
# Zephyr SDK, either omit CMAKE_SYSROOT (it finds headers via its prefix)
# or override it on the cmake command line.
if(EXISTS /usr/lib/arm-none-eabi)
    set(CMAKE_SYSROOT /usr/lib/arm-none-eabi)
endif()

# ── CMake search-path restrictions ──────────────────────────────────────────
# NEVER for programs: use host tools (cmake, python, etc.) as-is.
# BOTH for includes/libs: search both sysroot and standard locations so
# the compiler's own bundled headers (stdint.h in gcc's include/) are found
# alongside the newlib headers in the sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
