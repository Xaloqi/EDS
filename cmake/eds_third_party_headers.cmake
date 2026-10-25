# =============================================================================
# cmake/eds_third_party_headers.cmake — scope EDS's warnings to EDS code [#515]
#
# USAGE (from an example's CMakeLists.txt, after find_package(Zephyr), next to
# its target_compile_options(app PRIVATE ... -Wundef ...)):
#
#   include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/eds_third_party_headers.cmake)
#   eds_third_party_headers_are_system(app)
#
# WHY: the examples set -Wundef (MISRA C:2012 Rule 20.9) on the Zephyr `app`
# target, so GCC also applied it to every Zephyr, module and HAL header that
# EDS code includes. Those headers use undefined macros in #if on purpose
# (CONFIG_FLASH_MAP_LABELS, USE_HAL_RAMECC_REGISTER_CALLBACKS, ...), and with
# CONFIG_COMPILER_WARNINGS_AS_ERRORS=y the build failed inside code EDS does
# not own, on every Zephyr line.
#
# WHAT: every include directory Zephyr hands to the app (zephyr/include, the
# generated headers, modules, HAL, CMSIS) is also given to the target as a
# SYSTEM include. GCC then treats those headers as system headers: warnings
# raised inside them are not reported. EDS's own directories (core/, transport/,
# config/, platform/, generated/, the example's src/) are not touched, so
# -Wundef, -Wshadow, ... still apply to every EDS source and header, including
# EDS macros expanded in third-party code.
#
# MISRA evidence is not affected: Rule 20.9 evidence comes from
# misra_analysis.py's own host GCC/cppcheck pass over core/, transport/,
# config/ and generated/, not from these example builds.
# =============================================================================
function(eds_third_party_headers_are_system target)
    target_include_directories(${target} SYSTEM PRIVATE
        $<TARGET_PROPERTY:zephyr_interface,INTERFACE_INCLUDE_DIRECTORIES>)
endfunction()
