// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: tests/probe_eds_build_is_production.c
 *
 * PURPOSE: Preprocessor-only probe that proves EDS_BUILD_IS_PRODUCTION
 *          (core/uds_security_algo.h, SEC-BUILD-MODE-01) resolves to the
 *          correct value for every build-mode signal combination this
 *          codebase can present, across platforms.
 *
 * This file is never linked or run — it is compiled with -c under a set of
 * -D combinations by scripts/verify_build_mode_macro.sh, and the value of
 * EDS_BUILD_IS_PRODUCTION is extracted from a deliberate #error. gcc always
 * treats a triggered #error as a hard failure regardless of optimization or
 * warning flags, which makes this a reliable, permanent regression probe
 * for a compile-time-only macro that has no runtime representation to
 * assert against.
 *
 * TRACEABILITY: SEC-BUILD-MODE-01 / issue #84
 * ============================================================================= */

#include "uds_security_algo.h"

#if EDS_BUILD_IS_PRODUCTION
#error "EDS_BUILD_IS_PRODUCTION_PROBE_RESULT_1"
#else
#error "EDS_BUILD_IS_PRODUCTION_PROBE_RESULT_0"
#endif
