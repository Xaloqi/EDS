// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: transport/isotp.h
 *
 * PURPOSE: Public API for the ISO 15765-2 (ISO-TP) transport layer.
 *          Provides PDU segmentation and reassembly (SAR) for CAN frames,
 *          managing Single Frame (SF), First Frame (FF), Consecutive Frame (CF),
 *          and Flow Control (FC) frame types.
 *
 * SAFETY  : Transport layer integrity is prerequisite to UDS protocol correctness.
 *           ASIL-B candidate per ISO 26262-6.
 * TIMING  : As, Bs, Cr, Cs timing parameters are safety-relevant.
 * STANDARD: ISO 15765-2:2016 reference. MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef ISOTP_H
#define ISOTP_H

#include "uds_types.h"
#include "can_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * ISO-TP timing parameters (milliseconds)
 * ISO 15765-2:2016 Table 5
 * -------------------------------------------------------------------------- */

/**
 * As: Sender side timeout for the transmission confirmation of a SINGLE
 * CAN frame (ISO 15765-2:2016 §6.7.2, Table 5).
 *
 * N_As starts when one N_PDU is passed to the data link layer and stops on
 * that frame's transmission confirmation. It is NOT a whole-transfer
 * watchdog: the wait for a Flow Control frame is bounded by
 * ISOTP_TIMEOUT_BS_MS and the consecutive-frame cadence by STmin.
 *
 * "Transmission confirmation" is, at this layer, can_transport_transmit()
 * returning UDS_STATUS_OK — see the CONFIRMED contract documented on
 * transport/can_transport.h's can_transmit_fn typedef. Every platform CAN
 * HAL (can_transport_ops_t.transmit) must satisfy that contract for this
 * timer to mean what its name says.
 */
/* [EDS#453] ISO 15765-2:2024 Table 22: N_As, N_Ar, N_Bs and N_Cr time out
 * at 1000 ms, a lower limit (§9.8.1: "the real timeout shall occur no later
 * than specified timeout value +50 %"). EDS used 25/25/75/150 ms, which
 * aborted transfers with slower but conformant peers. The CONFIG_ symbols
 * (Kconfig) were never read before EDS#453/#455; they are now. */
#if !defined(ISOTP_TIMEOUT_AS_MS) && defined(CONFIG_ISOTP_TIMEOUT_AS_MS)
#define ISOTP_TIMEOUT_AS_MS   ((uint32_t)CONFIG_ISOTP_TIMEOUT_AS_MS)
#endif
#ifndef ISOTP_TIMEOUT_AS_MS
#define ISOTP_TIMEOUT_AS_MS   (1000U)
#endif

/**
 * Ar: Receiver side timeout for the transmission confirmation of a SINGLE
 * CAN frame (ISO 15765-2:2024 Table 22) — the exact receiver-side mirror of
 * N_As, with the same 1000 ms timeout.
 *
 * The only frame the receiver transmits during a reassembly is the Flow
 * Control frame, so N_Ar is the confirmation window of that FC. It starts
 * when the FC N_PDU is passed to the data link layer and stops on that
 * frame's transmission confirmation. It is NOT the wait for consecutive
 * frames — that window is ISOTP_TIMEOUT_CR_MS.
 *
 * As with N_As above, "transmission confirmation" means
 * can_transport_transmit() returning UDS_STATUS_OK under the CONFIRMED
 * contract in transport/can_transport.h.
 */
#if !defined(ISOTP_TIMEOUT_AR_MS) && defined(CONFIG_ISOTP_TIMEOUT_AR_MS)
#define ISOTP_TIMEOUT_AR_MS   ((uint32_t)CONFIG_ISOTP_TIMEOUT_AR_MS)
#endif
#ifndef ISOTP_TIMEOUT_AR_MS
#define ISOTP_TIMEOUT_AR_MS   (1000U)
#endif

/** Bs: Sender side timeout to receive a Flow Control frame
 *  (ISO 15765-2:2024 Table 22: 1000 ms). */
#if !defined(ISOTP_TIMEOUT_BS_MS) && defined(CONFIG_ISOTP_TIMEOUT_BS_MS)
#define ISOTP_TIMEOUT_BS_MS   ((uint32_t)CONFIG_ISOTP_TIMEOUT_BS_MS)
#endif
#ifndef ISOTP_TIMEOUT_BS_MS
#define ISOTP_TIMEOUT_BS_MS   (1000U)
#endif

/** Cr: Receiver side timeout between consecutive CAN frames
 *  (ISO 15765-2:2024 Table 22: 1000 ms). */
#if !defined(ISOTP_TIMEOUT_CR_MS) && defined(CONFIG_ISOTP_TIMEOUT_CR_MS)
#define ISOTP_TIMEOUT_CR_MS   ((uint32_t)CONFIG_ISOTP_TIMEOUT_CR_MS)
#endif
#ifndef ISOTP_TIMEOUT_CR_MS
#define ISOTP_TIMEOUT_CR_MS   (1000U)
#endif

/** Default STmin value in milliseconds (0x00 = send as fast as possible). */
#ifndef ISOTP_DEFAULT_STMIN_MS
#define ISOTP_DEFAULT_STMIN_MS (0U)
#endif

/**
 * Maximum number of payload bytes in a classical CAN Single Frame.
 * ISO 15765-2:2016 §9.6.1: classical CAN frame = 8 bytes total;
 * 1 byte consumed by the PCI (N_PCI) field, leaving 7 bytes for N_SDU data.
 *
 * FIX [H1]: Previously defined inside the #ifndef ISOTP_DEFAULT_STMIN_MS
 * guard block. If ISOTP_DEFAULT_STMIN_MS was defined externally (e.g. via
 * a compiler -D flag from prj.conf/Kconfig), the #ifndef block was skipped
 * entirely, making this constant invisible — any consumer would get a silent
 * zero or a compile error. Moved to its own unconditional #ifndef guard.
 */
#ifndef ISOTP_SF_MAX_PAYLOAD_LEN
#define ISOTP_SF_MAX_PAYLOAD_LEN (7U)
#endif

/**
 * Compile-time CAN FD opt-in.
 * Set to 1 (e.g. -DISOTP_ENABLE_CAN_FD=1) to enable the ISO 15765-2 §9.8
 * SF/FF escape paths.  Default 0 — Classic CAN only, FD code is compiled out
 * entirely so the binary contains no FD logic.
 *
 * The runtime use_fd flag in isotp_cfg_t / isotp_ctx_t is only present when
 * this macro is 1.  Both layers are required to enable CAN FD:
 *   - compile time: -DISOTP_ENABLE_CAN_FD=1
 *   - run time: isotp_cfg_t.use_fd = true
 */
#ifndef ISOTP_ENABLE_CAN_FD
#define ISOTP_ENABLE_CAN_FD 0
#endif

/**
 * Compile-time TX frame padding opt-in (ISO 15765-2 Annex B).
 * Set to 1 (e.g. -DISOTP_TX_PADDING=1) to pad all transmitted SF, FF
 * (CAN FD escape), CF, and FC frames with ISOTP_TX_PADDING_BYTE.
 * Classic CAN frames are padded to DLC 8; CAN FD frames are padded to
 * the next valid FD DLC (12, 16, 20, 24, 32, 48, or 64).
 * Default 0 — no padding, DLC reflects only the bytes actually used.
 *
 * Enable when interoperating with OEM diagnostic testers or bus analysers
 * that expect fixed-length frames (e.g. Vector CANalyzer, EOL testers).
 * Receivers MUST ignore padding bytes per ISO 15765-2 §8.
 *
 * Zephyr:          Add CONFIG_ISOTP_TX_PADDING=y to prj.conf.
 * FreeRTOS / bare-metal:  #define ISOTP_TX_PADDING 1 before including isotp.h,
 *                          or pass -DISOTP_TX_PADDING=1 to the compiler.
 */
/* [EDS#455] Honour the Kconfig symbol the comment above documents; before,
 * CONFIG_ISOTP_TX_PADDING=y was silently ignored. */
#if !defined(ISOTP_TX_PADDING) && defined(CONFIG_ISOTP_TX_PADDING)
#define ISOTP_TX_PADDING 1
#endif
#ifndef ISOTP_TX_PADDING
#define ISOTP_TX_PADDING 0
#endif

/**
 * Pad byte value used when ISOTP_TX_PADDING=1.
 * ISO 15765-2 Annex B recommends 0xCC as the conventional padding value.
 * Override with -DISOTP_TX_PADDING_BYTE=0xAA if your OEM tester requires
 * a different byte.
 */
#if !defined(ISOTP_TX_PADDING_BYTE) && defined(CONFIG_ISOTP_TX_PADDING_BYTE)
#define ISOTP_TX_PADDING_BYTE ((uint8_t)CONFIG_ISOTP_TX_PADDING_BYTE)
#endif
#ifndef ISOTP_TX_PADDING_BYTE
#define ISOTP_TX_PADDING_BYTE 0xCCU
#endif

#if ISOTP_ENABLE_CAN_FD
/**
 * Maximum payload bytes in a CAN FD Single Frame.
 * ISO 15765-2:2016 §9.8.1: CAN FD frame = up to 64 bytes;
 * 2 bytes consumed by the escape PCI (0x00 + SF_DL), leaving 62 bytes.
 */
#ifndef ISOTP_FD_SF_MAX_PAYLOAD_LEN
#define ISOTP_FD_SF_MAX_PAYLOAD_LEN (62U)
#endif
#endif /* ISOTP_ENABLE_CAN_FD */

/**
 * Static RX reassembly buffer size in bytes.
 * Defaults to UDS_MAX_PAYLOAD_LEN (4095).  Override at compile time
 * (e.g. -DISOTOP_RX_BUF_LEN=131072) for large CAN FD OTA transfers.
 * Must be at least 8 and no larger than UINT32_MAX.
 */
#ifndef ISOTP_RX_BUF_LEN
#define ISOTP_RX_BUF_LEN (UDS_MAX_PAYLOAD_LEN)
#endif

/**
 * [EDS#452] Receive-side Single Frame data padding check (ISO 15765-2:2024
 * §9.6.2.2, §11.3.2). A Classic CAN SF (CAN_DL <= 8) is accepted when:
 *   ISOTP_RX_PADDING_ANY       (0, default): CAN_DL == 8 (padded) or
 *                              CAN_DL == SF_DL + 1 (optimised);
 *   ISOTP_RX_PADDING_REQUIRED  (1): CAN_DL == 8 only;
 *   ISOTP_RX_PADDING_OPTIMISED (2): CAN_DL == SF_DL + 1 only.
 * Any other SF is ignored.
 */
#define ISOTP_RX_PADDING_ANY        (0)
#define ISOTP_RX_PADDING_REQUIRED   (1)
#define ISOTP_RX_PADDING_OPTIMISED  (2)
#if !defined(ISOTP_RX_PADDING_MODE) && defined(CONFIG_ISOTP_RX_PADDING_MODE)
#define ISOTP_RX_PADDING_MODE CONFIG_ISOTP_RX_PADDING_MODE
#endif
#ifndef ISOTP_RX_PADDING_MODE
#define ISOTP_RX_PADDING_MODE ISOTP_RX_PADDING_ANY
#endif

/** Largest FF_DL the 12-bit First Frame field encodes (ISO 15765-2 Table 16). */
#define ISOTP_FF_DL_12BIT_MAX  (4095U)

/** Largest 11-bit CAN identifier. */
#define ISOTP_CAN_STD_ID_MAX   (0x7FFU)

/** Default block size (0 = no block size limit). */
#ifndef ISOTP_DEFAULT_BLOCK_SIZE
#define ISOTP_DEFAULT_BLOCK_SIZE (0U)
#endif

/* --------------------------------------------------------------------------
 * ISO-TP frame type identifiers (N_PCItype, ISO 15765-2 Table 1)
 * -------------------------------------------------------------------------- */
#define ISOTP_FRAME_TYPE_SF  (0x0U) /**< Single Frame. */
#define ISOTP_FRAME_TYPE_FF  (0x1U) /**< First Frame. */
#define ISOTP_FRAME_TYPE_CF  (0x2U) /**< Consecutive Frame. */
#define ISOTP_FRAME_TYPE_FC  (0x3U) /**< Flow Control. */

/* --------------------------------------------------------------------------
 * Flow Control flow status values (ISO 15765-2 Table 8)
 * -------------------------------------------------------------------------- */
#define ISOTP_FC_STATUS_CONTINUE_TO_SEND  (0x00U) /**< CTS — receiver ready. */
#define ISOTP_FC_STATUS_WAIT              (0x01U) /**< WT — wait, send another FC. */
#define ISOTP_FC_STATUS_OVERFLOW          (0x02U) /**< OVFLW — buffer overflow. */

/* --------------------------------------------------------------------------
 * ISO-TP state machine states
 * -------------------------------------------------------------------------- */

/**
 * @brief ISO-TP channel state enumeration.
 */
typedef enum isotp_state {
    ISOTP_STATE_IDLE        = 0U, /**< No active transfer. */
    ISOTP_STATE_RX_WAIT_CF  = 1U, /**< Awaiting consecutive frames (RX). */
    ISOTP_STATE_TX_WAIT_FC  = 2U, /**< Awaiting flow control (TX). */
    ISOTP_STATE_TX_SEND_CF  = 3U, /**< Sending consecutive frames (TX). */
    ISOTP_STATE_ERROR       = 4U  /**< Error state — cleared by isotp_reset_*(); a TX-side error also by the next isotp_transmit() [EDS#541]. */
} isotp_state_t;

/* --------------------------------------------------------------------------
 * ISO-TP channel context
 * -------------------------------------------------------------------------- */

/**
 * @brief ISO-TP transport channel context.
 *
 * Holds all state required for one bidirectional ISO-TP logical channel.
 * SAFETY: Caller must not modify fields directly.
 */
typedef struct isotp_ctx {
    bool             initialized;                       /**< Initialization guard. */

    /* RX state */
    isotp_state_t    rx_state;                          /**< Current RX state. */
    uint8_t          rx_buf[ISOTP_RX_BUF_LEN];         /**< Static RX reassembly buffer. */
    uint32_t         rx_expected_len;                   /**< Total expected message length. */
    uint32_t         rx_received_len;                   /**< Bytes received so far. */
    uint8_t          rx_expected_sn;                    /**< Expected consecutive frame SN. */
    uint8_t          rx_blocks_received;                /**< [#121] CFs received in current block. */
    uint32_t         rx_cr_timer_ms;                    /**< Cr timeout countdown (ms). */
    uint32_t         rx_ar_timer_ms;                    /**< Ar timeout countdown (ms) — armed only across the FC frame's transmission confirmation. */
    uint8_t          rx_dl;                             /**< [EDS#452] RX_DL: CAN_DL of the FF of the reception in progress. */
    uds_addressing_t rx_addressing;                     /**< [EDS#444] Addressing of the PDU most recently passed to rx_cb. */
    uint8_t          rx_func_buf[UDS_CAN_FRAME_MAX_LEN]; /**< [EDS#445] Functional SF payload, kept apart from rx_buf. */

    /* TX state */
    isotp_state_t    tx_state;                          /**< Current TX state. */
    const uint8_t   *tx_data;                           /**< Pointer to TX data (not owned). */
    uint32_t         tx_total_len;                      /**< Total bytes to transmit. */
    uint32_t         tx_sent_len;                       /**< Bytes transmitted so far. */
    uint8_t          tx_sn;                             /**< TX consecutive frame sequence number. */
    uint8_t          tx_block_size;                     /**< Negotiated block size. */
    uint8_t          tx_stmin_ms;                       /**< Negotiated STmin in ms. */
    uint8_t          tx_blocks_sent;                    /**< [P2-TP-05] CFs sent in current block. */
    uint32_t         tx_stmin_timer_ms;                 /**< STmin countdown timer (ms). */
    uint32_t         tx_bs_timer_ms;                    /**< Bs timeout countdown (ms). */
    uint32_t         tx_as_timer_ms;                    /**< As timeout countdown (ms) — armed only across a single frame's transmission confirmation. */

    /* Configuration */
    uint32_t         rx_can_id;                         /**< CAN ID to receive on. */
    uint32_t         tx_can_id;                         /**< CAN ID to transmit on. */
    uint32_t         functional_rx_can_id;              /**< [EDS#444] Functional request CAN ID (0 = none). */
    uint32_t         periodic_tx_can_id;                /**< [EDS#546] UUDT periodic response CAN ID (0 = none). */
    uint8_t          local_block_size;                  /**< Block size to advertise in FC. */
    uint8_t          local_stmin_ms;                    /**< STmin to advertise in FC (ms). */
#if ISOTP_ENABLE_CAN_FD
    bool             use_fd;                            /**< True: CAN FD mode (SF up to 62 B, FF escape for >4095 B). */
#endif

    /* Bound CAN transport interface. */
    can_transport_t *can;                               /**< Pointer to CAN transport interface. */
} isotp_ctx_t;

/* --------------------------------------------------------------------------
 * ISO-TP configuration
 * -------------------------------------------------------------------------- */

/**
 * @brief Configuration block for an ISO-TP channel.
 */
typedef struct isotp_cfg {
    uint32_t         rx_can_id;         /**< CAN ID to listen for incoming frames. */
    uint32_t         tx_can_id;         /**< CAN ID to use for outgoing frames. */
    /**
     * [EDS#444/#445] CAN ID of functionally addressed requests (ISO 15765-4:
     * 0x7DF), or 0 for none: every frame is then treated as physical, as
     * before. When set, a frame received on this ID is functional: a Single
     * Frame is delivered without disturbing a physical reception in progress
     * (ISO 14229-1:2026 §7.7.6), and a First or Consecutive Frame is ignored.
     * The integrator passes the addressing on with isotp_get_rx_addressing()
     * and uds_server_process_request_ex().
     */
    uint32_t         functional_rx_can_id;
    /**
     * [EDS#546] CAN ID of the periodic data response messages of SID 0x2A
     * (ISO 14229-3:2022 §8.5, Table 3: UUDT messages "use a different CAN
     * identifier for periodic transmission"), or 0 for none. Used only by
     * isotp_transmit_uudt(); there is no implicit default.
     */
    uint32_t         periodic_tx_can_id;
    uint8_t          block_size;        /**< Block size to advertise to sender (0 = unlimited). */
    uint8_t          stmin_ms;          /**< Minimum separation time to advertise (ms). */
#if ISOTP_ENABLE_CAN_FD
    bool             use_fd;            /**< Set true to enable CAN FD SF/FF encoding. */
#endif
    can_transport_t *can;               /**< Pointer to initialized CAN transport interface. */
} isotp_cfg_t;

/* --------------------------------------------------------------------------
 * Receive completion callback
 * -------------------------------------------------------------------------- */

/**
 * @brief Callback invoked when a complete UDS PDU has been reassembled.
 *
 * @param[in] data    Pointer to the reassembled message data.
 * @param[in] length  Number of valid bytes in data[].
 * @param[in] arg     Caller-supplied opaque context pointer.
 *
 * @note TIMING: Callback executes in the diagnostics task context.
 *               Must not block or perform long operations.
 */
typedef void (*isotp_rx_complete_cb)(
    const uint8_t *data,
    uint32_t       length,
    void          *arg
);

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialize an ISO-TP channel context.
 *
 * @param[out] ctx  Caller-allocated ISO-TP context.
 * @param[in]  cfg  Channel configuration block.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if CAN IDs are zero or CAN interface is NULL.
 * @return UDS_STATUS_ERR_ALREADY_INITIALIZED if already initialized.
 */
uds_status_t isotp_init(isotp_ctx_t *ctx, const isotp_cfg_t *cfg);

/**
 * @brief Process an incoming raw CAN frame.
 *
 * Called by the CAN RX path for every received frame matching the channel CAN ID.
 * Internally manages SF/FF/CF/FC frame handling and reassembly state machine.
 *
 * @param[in] ctx         Initialized ISO-TP context.
 * @param[in] frame       Pointer to the received CAN frame.
 * @param[in] rx_cb       Callback to invoke when a complete PDU is ready.
 * @param[in] rx_cb_arg   Opaque argument passed to rx_cb.
 *
 * @return UDS_STATUS_OK on success or frame accepted.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if ctx not initialized.
 * @return UDS_STATUS_ERR_TP_FRAME_INVALID if frame format is not recognized.
 * @return UDS_STATUS_ERR_TP_OVERFLOW if RX buffer would be exceeded.
 * @return UDS_STATUS_ERR_TP_UNEXPECTED_PDU if frame received in invalid state.
 * @return UDS_STATUS_ERR_TP_TX_FAILED if the Flow Control frame this function
 *         had to transmit was rejected by the data link layer. The RX channel
 *         is left in ISOTP_STATE_ERROR and requires isotp_reset_rx() (or
 *         isotp_reset(), if tearing down both directions is acceptable).
 *         [#132] Note that this error is NOT observable through
 *         isotp_get_state() while a transmit is in progress — use
 *         isotp_get_rx_state(), or this return value.
 *
 * @note TIMING: Must complete within CAN frame inter-arrival time.
 */
uds_status_t isotp_process_rx_frame(
    isotp_ctx_t            *ctx,
    const uds_can_frame_t  *frame,
    isotp_rx_complete_cb    rx_cb,
    void                   *rx_cb_arg
);

/**
 * @brief Initiate transmission of a UDS PDU.
 *
 * Triggers SF or FF+CF segmented transmission depending on PDU length.
 * The pointed data must remain valid until transmission completes.
 *
 * [EDS#541] A TX channel left in ISOTP_STATE_ERROR (N_Bs or N_As timeout, a
 * Flow Control that aborted the message, a frame the data link refused) has
 * already reported that error and its message is lost, so this call starts the
 * new message from a cleared TX channel; it does not return ERR_BUSY. Calling
 * isotp_reset_tx() first is still allowed and does the same. A transmit that
 * is still in progress (TX_WAIT_FC, TX_SEND_CF) is never interrupted: that is
 * ERR_BUSY.
 *
 * @param[in] ctx     Initialized ISO-TP context.
 * @param[in] data    Pointer to PDU data to transmit (caller-owned).
 * @param[in] length  PDU length in bytes.
 *
 * @return UDS_STATUS_OK if transmission was initiated successfully.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if ctx not initialized.
 * @return UDS_STATUS_ERR_BUSY if a TX is already in progress.
 * @return UDS_STATUS_ERR_INVALID_PARAM if length is zero.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if length exceeds UDS_MAX_PAYLOAD_LEN
 *         (when ISOTP_ENABLE_CAN_FD=0, always; when =1, only if use_fd is false).
 *
 * @note TIMING: Timing-critical — TX initiation must occur within P2server_max.
 */
uds_status_t isotp_transmit(
    isotp_ctx_t    *ctx,
    const uint8_t  *data,
    uint32_t        length
);

/**
 * @brief [EDS#546] Send one UUDT message on isotp_cfg_t.periodic_tx_can_id.
 *
 * The periodic data response message of SID 0x2A (ISO 14229-1:2026 Table 221):
 * [periodicDataIdentifier, dataRecord...]. ISO 14229-3:2022 §8.5 (Table 3,
 * REQ 7.5/7.7/7.13): an unsegmented message on its own CAN ID, with neither a
 * protocol control information byte nor a service ID, written directly to the
 * CAN driver and bypassing the network layer. So this does not use, change or
 * wait for the ISO-TP TX channel: it is allowed while a multi-frame response is
 * being sent. The DLC is the length (no padding).
 *
 * @param[in] ctx     Initialized ISO-TP context.
 * @param[in] data    pDID followed by the data record.
 * @param[in] length  1..8 (classic CAN; the periodic DID's record is at most 7 bytes).
 *
 * @return UDS_STATUS_OK once the frame is confirmed transmitted.
 * @return UDS_STATUS_ERR_NULL_PTR / ERR_NOT_INITIALIZED as isotp_transmit().
 * @return UDS_STATUS_ERR_INVALID_PARAM if length is zero or no periodic CAN ID is configured.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if length exceeds 8.
 * @return UDS_STATUS_ERR_TP_TX_FAILED if the CAN transport refused the frame.
 */
uds_status_t isotp_transmit_uudt(
    isotp_ctx_t    *ctx,
    const uint8_t  *data,
    uint32_t        length
);

/**
 * @brief 1 ms periodic tick for ISO-TP timer management.
 *
 * Drives Cr, Ar, Bs, and As timeout timers. Transitions channel to
 * ISOTP_STATE_ERROR on timeout expiry.
 *
 * @note As is scoped to one frame's transmission confirmation and is stopped
 *       by the TX path as soon as can_transport_transmit() returns, so it does
 *       not bound the FC wait (Bs) or a multi-frame CF sequence (STmin). This
 *       is only meaningful because can_transport_transmit() is CONFIRMED, not
 *       queued — see transport/can_transport.h's can_transmit_fn contract.
 * @note Ar is the receiver-side mirror of As, scoped to the Flow Control
 *       frame's transmission confirmation and stopped by isotp_send_fc() as
 *       soon as can_transport_transmit() returns. It does not bound the wait
 *       for consecutive frames (Cr).
 *
 * @param[in] ctx  Initialized ISO-TP context.
 *
 * @return UDS_STATUS_OK if no timeout occurred.
 * @return UDS_STATUS_ERR_TP_TIMEOUT_CR if Cr timer expired.
 * @return UDS_STATUS_ERR_TP_TIMEOUT_AS if As timer expired.
 * @return UDS_STATUS_ERR_TP_TIMEOUT_AR if Ar timer expired (FC confirmation).
 * @return UDS_STATUS_ERR_TP_TIMEOUT_BS if Bs timer expired (no FC received).
 * @return UDS_STATUS_ERR_NULL_PTR if ctx is NULL.
 *
 * @note TIMING: Must be called at 1 ms resolution.
 * @note SAFETY: ASIL-B relevant — timeout expiry triggers error state.
 */
uds_status_t isotp_tick_1ms(isotp_ctx_t *ctx);

/**
 * @brief Reset BOTH directions of the ISO-TP channel to IDLE state.
 *
 * Clears all RX and TX state. May be called after error recovery.
 *
 * Exactly equivalent to isotp_reset_rx() followed by isotp_reset_tx(); it is
 * implemented as such so the two never drift apart.
 *
 * @warning [#132] This tears down BOTH directions. Recovering an RX error
 *          with it aborts any transmit still in flight (and vice versa). To
 *          recover one direction only, use isotp_reset_rx() /
 *          isotp_reset_tx().
 *
 * @param[in] ctx  Initialized ISO-TP context.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if ctx is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if ctx is not initialized.
 */
uds_status_t isotp_reset(isotp_ctx_t *ctx);

/**
 * @brief [#132] Reset ONLY the receive direction to IDLE state.
 *
 * Clears rx_state, the reassembly length/sequence counters, the RX block
 * counter and the Cr/Ar timers. An in-flight transmit — tx_state, tx_data,
 * the CF sequence number, the negotiated BS/STmin and the As/Bs/STmin timers
 * — is left completely untouched and continues normally.
 *
 * This is the targeted recovery from an RX-side ISOTP_STATE_ERROR (a Cr or Ar
 * timeout, a sequence-number or DLC fault, a buffer overflow, or the
 * UDS_STATUS_ERR_TP_TX_FAILED reported by isotp_process_rx_frame() when a
 * Flow Control frame is rejected by the data link layer).
 *
 * SAFETY: the two directions share no mutable state — only the read-only
 * configuration and the bound can_transport_t — so this leaves the context
 * fully valid for every other API function. Afterwards a new First Frame is
 * accepted normally and a stray Consecutive Frame is rejected with
 * UDS_STATUS_ERR_TP_UNEXPECTED_PDU, exactly as from a freshly initialized
 * channel.
 *
 * @note The reassembly buffer contents are not scrubbed (neither does
 *       isotp_reset()); rx_received_len is zeroed, so no stale byte is
 *       reachable.
 * @note This is a local teardown only. It does not signal the peer, which
 *       may still be sending consecutive frames for the abandoned PDU; those
 *       are rejected as unexpected PDUs. Same semantics as isotp_reset().
 *
 * @param[in] ctx  Initialized ISO-TP context.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if ctx is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if ctx is not initialized.
 */
uds_status_t isotp_reset_rx(isotp_ctx_t *ctx);

/**
 * @brief [#132] Reset ONLY the transmit direction to IDLE state.
 *
 * Clears tx_state, the borrowed tx_data pointer, the length/sequence
 * counters, the negotiated block size and STmin, and the As/Bs/STmin timers.
 * An in-progress reassembly — rx_state, the RX buffer and its counters, and
 * the Cr/Ar timers — is left completely untouched and continues normally.
 *
 * This is the targeted recovery from a TX-side ISOTP_STATE_ERROR (a Bs or As
 * timeout, a Flow Control OVERFLOW or reserved flow-status abort, or a
 * consecutive frame rejected by the data link layer).
 *
 * SAFETY: see isotp_reset_rx(). Dropping the tx_data pointer here also
 * releases the caller's obligation to keep that buffer alive.
 *
 * @note This is a local teardown only. It does not signal the peer, which may
 *       still be awaiting the remainder of the aborted PDU. Same semantics as
 *       isotp_reset().
 *
 * @param[in] ctx  Initialized ISO-TP context.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if ctx is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if ctx is not initialized.
 */
uds_status_t isotp_reset_tx(isotp_ctx_t *ctx);

/**
 * @brief Query the current channel state (direction-collapsing).
 *
 * @warning [#132] This accessor reports ONE state for a context that runs TWO
 *          independent state machines. It returns tx_state whenever a
 *          transmit is in progress and rx_state only otherwise, so an RX
 *          channel sitting in ISOTP_STATE_ERROR is invisible here for the
 *          entire duration of any concurrent transmit. On a UDS server the
 *          transmit direction is busy for exactly as long as a segmented
 *          response is going out, which is precisely when an inbound request
 *          is being reassembled.
 *
 *          Retained unchanged for source compatibility. New code should use
 *          isotp_get_rx_state() and isotp_get_tx_state(), which report each
 *          direction faithfully.
 *
 * @param[in]  ctx        Initialized ISO-TP context.
 * @param[out] out_state  Receives tx_state if it is not ISOTP_STATE_IDLE,
 *                        otherwise rx_state.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if ctx is not initialized.
 */
uds_status_t isotp_get_state(
    const isotp_ctx_t *ctx,
    isotp_state_t     *out_state
);

/**
 * @brief [#132] Query the receive-direction state, unaliased.
 *
 * Reports rx_state exactly, whether or not a transmit is in progress. This is
 * the only way to observe an RX-side ISOTP_STATE_ERROR concurrently with a
 * transmit; isotp_get_state() hides it.
 *
 * @param[in]  ctx        Initialized ISO-TP context.
 * @param[out] out_state  Pointer to receive the current RX state.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if ctx is not initialized.
 */
uds_status_t isotp_get_rx_state(
    const isotp_ctx_t *ctx,
    isotp_state_t     *out_state
);

/**
 * @brief [EDS#444] Addressing of the PDU most recently passed to rx_cb.
 *
 * Call it from inside rx_cb, and hand the result to
 * uds_server_process_request_ex(). UDS_ADDRESSING_FUNCTIONAL only when
 * isotp_cfg_t.functional_rx_can_id is set and the PDU arrived on it.
 *
 * @param[in]  ctx             Initialized ISO-TP context.
 * @param[out] out_addressing  Receives the addressing type.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if ctx is not initialized.
 */
uds_status_t isotp_get_rx_addressing(
    const isotp_ctx_t *ctx,
    uds_addressing_t  *out_addressing
);

/**
 * @brief [#132] Query the transmit-direction state, unaliased.
 *
 * Reports tx_state exactly. Unlike isotp_get_state(), an ISOTP_STATE_IDLE
 * result here unambiguously means "no transmit in progress" rather than
 * "no transmit in progress AND the receiver is also idle".
 *
 * @param[in]  ctx        Initialized ISO-TP context.
 * @param[out] out_state  Pointer to receive the current TX state.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if ctx is not initialized.
 */
uds_status_t isotp_get_tx_state(
    const isotp_ctx_t *ctx,
    isotp_state_t     *out_state
);

#ifdef __cplusplus
}
#endif

#endif /* ISOTP_H */
