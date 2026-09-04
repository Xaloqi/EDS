/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: GENERATED — dtc_config.h
 *
 * ECU       : ARDEP_IOController
 * Version   : 1.0.0
 * Generated : 2026-09-04T13:51:27Z
 *
 * PURPOSE: Per-example DTC table, exposed as an X-macro list so any
 *          translation unit that already links against this example's
 *          generated/ sources (e.g. harness/harness_ecu.c) can register
 *          this example's DTCs without a per-example switch statement —
 *          the same "compile against examples/<name>/generated/" mechanism
 *          did_handlers.c and routine_handlers.c already use (issue #158).
 *
 *          Architecture:
 *            diagnostics_config.yaml (dtcs section)
 *              -> tools/codegen.py (build_dtc_config_context)
 *                -> tools/templates/dtc_config.h.j2
 *                  -> generated/dtc_config.h  (this file)
 *
 *          NOTE: codegen.py's RENDER_PLAN lives in the public EDS repo.
 *          Wiring this template into that RENDER_PLAN is tracked as a
 *          follow-up there; until then this file is kept in sync by hand
 *          from each example's own generated/uds_init.c Step 5 block (the
 *          canonical, already-codegen-produced source of truth for that
 *          example's DTCs) — same content, just re-exposed as an X-macro
 *          list so harness_ecu.c can consume it generically.
 *
 * USAGE:
 *   #define REGISTER_ONE_DTC(code, severity, desc) \
 *       (void)dtc_database_register((uint32_t)(code), (uint8_t)(severity), (desc));
 *   GEN_DTC_TABLE(REGISTER_ONE_DTC)
 *   #undef REGISTER_ONE_DTC
 *
 * WARNING: DO NOT EDIT MANUALLY once codegen.py renders this template.
 *          Until then, regenerate by hand from this example's
 *          generated/uds_init.c Step 5 block if diagnostics_config.yaml's
 *          dtcs: section changes.
 *
 * SAFETY  : DTC codes and severities govern SID 0x19 (ReadDTCInformation)
 *           and SID 0x14 (ClearDiagnosticInformation) responses.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef DTC_CONFIG_H
#define DTC_CONFIG_H

/*
 * X-macro list of this example's DTCs: (code, severity byte, description).
 * Severity encoding (SAE J2012-DA): 0x20 = check_at_next_halt, 0x40 = other.
 * Invoke as GEN_DTC_TABLE(X) with X(code, severity, desc) defined by the
 * caller; #undef X immediately after use.
 */
#define GEN_DTC_TABLE(X) \
    X((uint32_t)12583168UL, (uint8_t)0x20U, "CAN bus communication loss — no frames received > 500ms") \
    X((uint32_t)12583424UL, (uint8_t)0x20U, "CAN bus error passive — TX/RX error counters exceeded 127") \
    X((uint32_t)12583680UL, (uint8_t)0x20U, "CAN bus off — transmitter disabled by bus-off recovery") \
    X((uint32_t)12587008UL, (uint8_t)0x40U, "LIN bus no response — no slave response within timeout") \
    X((uint32_t)12587264UL, (uint8_t)0x40U, "LIN bus framing error — break field or sync byte invalid") \
    X((uint32_t)11534592UL, (uint8_t)0x20U, "Output 1 overcurrent — load current exceeded threshold") \
    X((uint32_t)11534848UL, (uint8_t)0x20U, "Output 2 overcurrent — load current exceeded threshold") \
    X((uint32_t)11535104UL, (uint8_t)0x20U, "Output 3 overcurrent — load current exceeded threshold") \
    X((uint32_t)11535360UL, (uint8_t)0x20U, "Output 4 overcurrent — load current exceeded threshold") \
    X((uint32_t)11535616UL, (uint8_t)0x20U, "Output 5 overcurrent — load current exceeded threshold") \
    X((uint32_t)11535872UL, (uint8_t)0x20U, "Output 6 overcurrent — load current exceeded threshold") \
    X((uint32_t)11538432UL, (uint8_t)0x40U, "PowerIO open load — output commanded ON but no current detected") \
    X((uint32_t)12648192UL, (uint8_t)0x20U, "Supply voltage low — below 9.0V for > 500ms") \
    X((uint32_t)12648208UL, (uint8_t)0x20U, "Supply voltage high — above 16.0V for > 100ms") \
    X((uint32_t)12648224UL, (uint8_t)0x20U, "ECU overtemperature — internal junction > 125°C") \
    X((uint32_t)12648240UL, (uint8_t)0x40U, "Watchdog reset event — firmware failed to service watchdog") \
    X((uint32_t)12648256UL, (uint8_t)0x40U, "NVM write failure — diagnostic session statistics not persisted") \
    X((uint32_t)12647936UL, (uint8_t)0x20U, "Firmware image verification failed — CRC mismatch") \
    X((uint32_t)12647952UL, (uint8_t)0x40U, "Firmware download incomplete — transfer interrupted")
#endif /* DTC_CONFIG_H */
