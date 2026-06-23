/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: GENERATED — uds_init.h
 *
 * ECU       : SensorECU
 * Version   : 1.0.0
 * Generated : 2026-06-23T19:16:14Z
 *
 * PURPOSE: Public interface for the generated UDS stack initialisation module.
 *
 * WARNING: DO NOT EDIT MANUALLY.
 *          Regenerate: python3 tools/codegen.py --config <yaml> --out generated/
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef UDS_INIT_H
#define UDS_INIT_H

#include "uds_types.h"
#include "uds_server.h"
#include "uds_safety.h"
#include "safety_config.h"
#ifndef EDS_DOIP_ONLY_BUILD
#include "isotp.h"
#include "can_transport.h"
#endif /* EDS_DOIP_ONLY_BUILD */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the complete UDS diagnostics stack.
 *
 * Performs the following in order:
 *   1. did_database_init()
 *   2. dtc_database_init()
 *   3. did_handlers_register_all()   — generated DID table
 *   4. uds_session_init()            — with GEN_S3_SERVER_TIMEOUT_MS
 *   5. uds_security_init()           — with application-provided callbacks
 *   6. uds_server_init()             — with GEN_P2_SERVER_MAX_MS and service table
 *   7. isotp_init()                  — bound to the provided CAN transport
 *
 * Must be called exactly once from the application task before entering
 * the main diagnostics poll loop.
 *
 * @param[in] can         Pointer to an initialized CAN transport instance.
 *                        Must remain valid for the lifetime of the stack.
 * @param[in] rx_can_id   CAN ID on which the ECU listens for diagnostic requests.
 * @param[in] tx_can_id   CAN ID on which the ECU transmits diagnostic responses.
 *
 * @return UDS_STATUS_OK if all modules initialized successfully.
 * @return UDS_STATUS_ERR_NULL_PTR if can is NULL.
 * @return UDS_STATUS_ERR_* propagated from any failing sub-initialisation.
 *
 * @note SAFETY: Must be called from initialisation context, not from ISR.
 */
#ifndef EDS_DOIP_ONLY_BUILD
uds_status_t uds_generated_init(
    can_transport_t *can,
    uint32_t         rx_can_id,
    uint32_t         tx_can_id
);
#else
/* DoIP-only build: can parameter is unused (pass NULL). */
uds_status_t uds_generated_init(
    void    *can,
    uint32_t rx_can_id,
    uint32_t tx_can_id
);
#endif /* EDS_DOIP_ONLY_BUILD */

/**
 * @brief Return a pointer to the generated UDS server context.
 *
 * Allows the application poll loop to call uds_server_process_request()
 * and uds_server_tick_1ms() without needing direct access to the context.
 *
 * @return Non-NULL pointer to the statically allocated server context.
 *         Returns NULL if uds_generated_init() has not been called.
 */
uds_server_ctx_t *uds_generated_get_server(void);

/**
 * @brief Return a pointer to the generated ISO-TP context.
 *
 * Allows the application poll loop to call isotp_process_rx_frame(),
 * isotp_transmit(), and isotp_tick_1ms().
 *
 * @return Non-NULL pointer to the statically allocated ISO-TP context.
 *         Returns NULL if uds_generated_init() has not been called.
 */
#ifndef EDS_DOIP_ONLY_BUILD
isotp_ctx_t *uds_generated_get_isotp(void);
#else
/* DoIP-only build: returns NULL, isotp_ctx_t unavailable */
static inline void *uds_generated_get_isotp(void) { return ((void *)0); }
#endif /* EDS_DOIP_ONLY_BUILD */

/**
 * @brief Application-provided security seed generation callback.
 *
 * This function MUST be implemented by the application. It is declared here
 * and called from the generated uds_init.c during security initialisation.
 *
 * @param[in]  security_level  Seed sub-function value (odd: 0x01, 0x03, ...).
 * @param[out] seed_buf        Buffer to receive generated seed bytes.
 * @param[in]  seed_buf_len    Size of seed_buf.
 * @param[out] out_seed_len    Number of seed bytes written.
 *
 * @return UDS_STATUS_OK on success.
 *
 * @note SAFETY: Safety-critical. Must be replaced with OEM-approved TRNG-seeded
 *               algorithm before any vehicle deployment.
 */
uds_status_t app_security_seed_generate(
    uint8_t  security_level,
    uint8_t *seed_buf,
    uint8_t  seed_buf_len,
    uint8_t *out_seed_len
);

/**
 * @brief Application-provided security key validation callback.
 *
 * This function MUST be implemented by the application.
 *
 * @param[in] security_level  Security level sub-function identifier.
 * @param[in] seed            Seed previously sent to the tester.
 * @param[in] seed_len        Seed length in bytes.
 * @param[in] key             Key received from the tester.
 * @param[in] key_len         Key length in bytes.
 *
 * @return true if key is valid for the given seed and level.
 * @return false otherwise.
 *
 * @note SAFETY: Safety-critical. Algorithm must be reviewed by security team.
 */
bool app_security_key_validate(
    uint8_t        security_level,
    const uint8_t *seed,
    uint8_t        seed_len,
    const uint8_t *key,
    uint8_t        key_len
);

#ifdef __cplusplus
}
#endif

#endif /* UDS_INIT_H */
