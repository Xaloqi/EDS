/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: GENERATED — generated_config.h
 *
 * ECU       : BasicECU_DoIP_FreeRTOS
 * Version   : 1.6.0
 * Generated : 2026-05-20T07:21:47Z
 *
 * PURPOSE: Compile-time constants derived from diagnostics_config.yaml.
 *          Provides all protocol timing parameters, CAN addressing, and
 *          database dimension bounds used across the diagnostics stack.
 *
 *          Architecture:
 *            diagnostics_config.yaml
 *              -> tools/codegen.py (build_generated_config_context)
 *                -> tools/templates/generated_config.h.j2
 *                  -> generated/generated_config.h  (this file)
 *
 *          This design allows all timing, addressing, and sizing constants
 *          to be driven from a single YAML source without touching C headers.
 *
 * WARNING: DO NOT EDIT MANUALLY.
 *          Regenerate: python3 tools/codegen.py --config <yaml> --out generated/
 *
 * SAFETY  : Timing constants govern ISO 14229-1 protocol compliance.
 *           CAN IDs govern which frames the ECU accepts and transmits.
 *           Changing these values without revalidating timing analysis is unsafe.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef GENERATED_CONFIG_H
#define GENERATED_CONFIG_H

/* --------------------------------------------------------------------------
 * Protocol timing constants
 *
 * All values in milliseconds. Derived from the 'timing' section of
 * diagnostics_config.yaml. ISO 14229-1 §6.2 reference values shown.
 * -------------------------------------------------------------------------- */

/*
 * P2server_max — Maximum response time before ECU must respond or send
 * NRC 0x78 (requestCorrectlyReceivedResponsePending).
 * ISO 14229-1 default: 50 ms. YAML value: 25 ms.
 */
#define GEN_P2_SERVER_MAX_MS      (25U)

/*
 * P2*server_max — Extended response time after NRC 0x78 ResponsePending.
 * ISO 14229-1 default: 5000 ms. YAML value: 5000 ms.
 * Constraint: GEN_P2_STAR_SERVER_MAX_MS > GEN_P2_SERVER_MAX_MS.
 */
#define GEN_P2_STAR_SERVER_MAX_MS (5000U)

/*
 * S3server — Session inactivity timeout. After this duration without a
 * TesterPresent or any other request, the ECU reverts to Default Session.
 * ISO 14229-1 default: 5000 ms. YAML value: 5000 ms.
 */
#define GEN_S3_SERVER_TIMEOUT_MS  (5000U)

/* --------------------------------------------------------------------------
 * CAN transport addressing
 *
 * Derived from the 'can' section of diagnostics_config.yaml.
 * Defaults: 0x7DF (ISO 15765-4 functional RX), 0x7E8 (physical TX ECU #0).
 * -------------------------------------------------------------------------- */

/** CAN ID on which the ECU listens for UDS diagnostic requests. */
#define GEN_CAN_RX_ID             (2015U)   /* 0x7DF */

/** CAN ID on which the ECU transmits UDS diagnostic responses. */
#define GEN_CAN_TX_ID             (2024U)   /* 0x7E8 */

/** True (1) if RX CAN ID is a 29-bit extended identifier. */
#define GEN_CAN_RX_IS_EXTENDED    (0U)

/** True (1) if TX CAN ID is a 29-bit extended identifier. */
#define GEN_CAN_TX_IS_EXTENDED    (0U)

/* --------------------------------------------------------------------------
 * Database dimensions
 *
 * Must not exceed UDS_MAX_DID_COUNT and UDS_MAX_DTC_COUNT in uds_types.h.
 * These macros allow compile-time array sizing in generated code.
 * -------------------------------------------------------------------------- */

/** Number of DIDs declared in diagnostics_config.yaml. */
#define GEN_DID_COUNT             (5U)

/** Number of DTCs declared in diagnostics_config.yaml. */
#define GEN_DTC_COUNT             (2U)

/* --------------------------------------------------------------------------
 * ECU identification (tooling and logging use only)
 *
 * NOT intended for use in protocol responses. Use dedicated DIDs
 * (e.g. 0xF190 VIN, 0xF18C ECU Serial) for protocol-level identification.
 * -------------------------------------------------------------------------- */

#define GEN_ECU_NAME            "BasicECU_DoIP_FreeRTOS"
#define GEN_ECU_VERSION         "1.6.0"
#define GEN_GENERATED_TIMESTAMP "2026-05-20T07:21:47Z"

#endif /* GENERATED_CONFIG_H */
