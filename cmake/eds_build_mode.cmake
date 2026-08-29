# =============================================================================
# cmake/eds_build_mode.cmake — FreeRTOS build-mode declaration [SEC-BUILD-MODE-01]
#
# USAGE (from any FreeRTOS example CMakeLists.txt, after DIAG_ROOT is set):
#
#   include(${DIAG_ROOT}/cmake/eds_build_mode.cmake)
#   target_compile_definitions(<target> PRIVATE
#       CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=$<BOOL:${EDS_PLACEHOLDER_KEYS_ONLY}>
#   )
#
# WHY THIS FILE EXISTS (issue #84):
#   FreeRTOS has no Kconfig of its own, so without this option
#   CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY is never defined for a FreeRTOS build,
#   and EDS_BUILD_IS_PRODUCTION (core/uds_security_algo.h) falls through to
#   its PRODUCTION default on every build, including CI. This option mirrors
#   Zephyr Kconfig's own `default y` for the same symbol: ON here means
#   "development / CI build, placeholder keys explicitly permitted."
#
#   Previously each FreeRTOS example would have needed this option()
#   declared identically in its own CMakeLists.txt. That is exactly the
#   pattern cmake/eds_service_sources.cmake's own header warns about:
#   per-example duplication of shared, security-relevant configuration
#   causing real bugs when one example is missed on a future change.
#
# FLIPPING TO PRODUCTION:
#   -DEDS_PLACEHOLDER_KEYS_ONLY=OFF declares a production build. This is
#   what makes the CRIT-4 #error (core/uds_security_algo.c) and the Step 7.1
#   runtime guard (generated/uds_init.c) reachable on FreeRTOS at all.
#
# ADDING A NEW FREERTOS EXAMPLE:
#   include() this file and wire the one target_compile_definitions() line
#   above. No other change required — this option is shared, not per-example.
# =============================================================================

option(EDS_PLACEHOLDER_KEYS_ONLY "Allow placeholder AES-128 security keys (DEVELOPMENT ONLY)" ON)
