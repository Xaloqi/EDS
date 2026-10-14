# =============================================================================
# cmake/eds_zephyr_module.cmake — load EDS's Kconfig from THIS tree [#414]
#
# USAGE (from any Zephyr application CMakeLists.txt, BEFORE find_package):
#
#   include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/eds_zephyr_module.cmake)
#   find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
#
# WHY THIS FILE EXISTS (issue #414):
#   EDS's Kconfig (top-level Kconfig, registered by zephyr/module.yml) only
#   reaches a build when Zephyr loads EDS as a module. The examples compile
#   EDS sources by relative path from their own checkout, but take Kconfig
#   from whatever module named xaloqi-eds the west workspace provides:
#
#     - EDS is the manifest repo (CI, docs/GETTING_STARTED.md): same tree,
#       consistent.
#     - EDS checked out alongside another workspace (ARDEP, a bench): no EDS
#       module at all. Every prj.conf assignment to CONFIG_ISOTP_*,
#       CONFIG_UDS_* or CONFIG_DIAG_* is an undefined-symbol error, and
#       configdefaults such as NET_CONTEXT_SNDTIMEO (#405) never apply.
#     - The workspace carries a different EDS checkout as its module: Kconfig
#       comes from that version, silently out of step with the sources.
#
#   Appending this tree to ZEPHYR_EXTRA_MODULES fixes all three. Zephyr's
#   scripts/zephyr_module.py keys modules by name and processes extra modules
#   last, so this tree replaces any workspace copy named xaloqi-eds. When the
#   workspace copy IS this tree (CI) nothing changes.
#
#   Only Kconfig is affected: zephyr/module.yml has no cmake: entry, so this
#   adds no sources.
# =============================================================================

get_filename_component(EDS_MODULE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
list(APPEND ZEPHYR_EXTRA_MODULES "${EDS_MODULE_ROOT}")
