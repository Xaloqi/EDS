// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: config/dtc_mirror.h
 *
 * PURPOSE: DTC NVM mirror — persist and restore DTC status bytes across resets.
 *
 *          DTC status bytes reside in the RAM-based dtc_database (s_dtc_table).
 *          Without persistence, all fault history is lost on ECU reset or
 *          power-cycle — contrary to ISO 14229-1:2026 Annex D.2 (Table D.5: confirmed DTCs are kept in long-term memory) for
 *          ConfirmedDTC and TestFailedSinceLastClear status bits.
 *
 *          This module:
 *            1. Loads the DTC mirror from NVM on boot (dtc_mirror_load).
 *            2. Writes a monitor-path status change to NVM within a bounded
 *               delay (dtc_mirror_tick_1ms, see WHAT IS GUARANTEED).
 *            3. Writes at once at service 0x14 (ClearDiagnosticInformation)
 *               and before a controlled ECU reset (0x11).
 *
 *          Wire points:
 *            - dtc_mirror_load()        → called from uds_stack_init()
 *            - dtc_mirror_tick_1ms()    → called from uds_server_tick_1ms()
 *            - dtc_mirror_save_one()    → called from SID 0x14 (single-DTC clear)
 *            - dtc_mirror_flush_all()   → called from zephyr_port_nvm_flush()
 *
 * WHAT IS GUARANTEED [EDS#632]:
 *   - A dtc_database_set_status() call that changes ConfirmedDTC or
 *     TestFailedSinceLastClear (DTC_PERSIST_TRIGGER_MASK) is written to NVM
 *     by uds_server_tick_1ms() at most DTC_MIRROR_FLUSH_INTERVAL_MS after
 *     the change, plus the time of one NVM write, provided the server is
 *     ticked, the mirror is initialised and the NVM write succeeds. A power
 *     loss, watchdog reset or reset pin inside that window loses that
 *     change; after it, the change survives.
 *   - A failed write is retried at the next interval; the change is not
 *     dropped.
 *   - 0x14 and the flush before a controlled reset (0x11) still write at
 *     once.
 *   - The other status bits are written with the whole byte at the next
 *     write, but do not cause one. The permanent flag is not persisted, by
 *     decision (EDS#580): the application marks a restored DTC permanent
 *     again.
 *   - Flash wear: at most one mirror write (the mirror record, then the
 *     first/most recent records) per DTC_MIRROR_FLUSH_INTERVAL_MS, however
 *     often the bits change. A conforming monitor sets ConfirmedDTC once
 *     per fault and only 0x14 (or aging) clears it, so the real rate is
 *     about one write per new fault. An application that clears and sets
 *     the bits on every debounce cycle reaches the cap: size the NVM
 *     partition for it or raise the interval.
 *   - Time: the write runs in the thread that ticks the server, under the
 *     integration's UDS locks, never in the caller of
 *     dtc_database_set_status() (that only sets a flag under the DTC
 *     lock). It costs what a 0x14 already costs: one NVM write, which can
 *     include a sector erase when the NVM backend garbage-collects.
 *
 * WIRE FORMAT (NVM_KEY_DTC_MIRROR) — v1, integrity-checked (issue #114):
 *   [magic:2][version:1][count:2][entry_0:4]...[entry_n:4][crc32:4]
 *   Each entry: [dtc_code:3 big-endian][status_byte:1]
 *   magic   = 0x44,0x4D ("DM"). Identifies a v1-or-later record.
 *   version = DTC_MIRROR_FORMAT_VERSION. Bump on any layout change.
 *   count   = number of entries, big-endian. Bounded by
 *             DTC_MIRROR_MAX_PERSISTED_DTCS (issue #123), NOT by
 *             UDS_MAX_DTC_COUNT — see that constant's comment below.
 *   crc32   = CRC-32 (same polynomial/algorithm as the transfer-service
 *             helper in core/uds_transfer_ctx.c) computed over
 *             [version..last entry byte] (i.e. everything except the
 *             magic and the CRC field itself), big-endian.
 *   Maximum payload: 5 + DTC_MIRROR_MAX_PERSISTED_DTCS(125) × 4 + 4 = 509 bytes.
 *   This is DTC_MIRROR_MAX_BYTES, held by a build-time _Static_assert in
 *   dtc_mirror.c to be <= platform/nvm_store.h's NVM_MAX_RECORD_BYTES (512).
 *   Sizing this from UDS_MAX_DTC_COUNT (128) instead — 5+128×4+4 = 521 —
 *   was the issue #123 bug: 9 bytes over the NVM cap, so nvm_store_write()
 *   silently rejected the mirror write once the live DTC table grew past
 *   125 entries. dtc_database itself still holds up to UDS_MAX_DTC_COUNT
 *   (128) DTCs; entries at index >= DTC_MIRROR_MAX_PERSISTED_DTCS simply
 *   are not persisted — a documented limitation, not a truncated write.
 *
 * INTEGRITY (issue #114 fix):
 *   Before this fix, the mirror was [count:2][entries...] with no way to
 *   tell a truncated/corrupted record from a legitimately short one — a
 *   short read mid-loop silently applied only the entries it had parsed
 *   and still returned UDS_STATUS_OK. dtc_mirror_load() now validates the
 *   whole record (magic, version, declared length, CRC-32) BEFORE applying
 *   any entry to the live database, so a corrupt record can never partially
 *   apply. See dtc_mirror_load() below for the exact case breakdown and the
 *   deliberate legacy-record migration decision.
 *
 * SAFETY  : ASIL-B candidate. Confirmed DTC bits are safety-relevant.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef DTC_MIRROR_H
#define DTC_MIRROR_H

#include "uds_types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Wire format constants
 * -------------------------------------------------------------------------- */

/** Magic bytes identifying a v1-or-later (integrity-checked) mirror record.
 *  Chosen so a legacy pre-header record's leading count field can never
 *  collide with it: a legacy count is bounded by UDS_MAX_DTC_COUNT (128),
 *  so its high byte is always 0x00, whereas DTC_MIRROR_MAGIC_0 is not. */
#define DTC_MIRROR_MAGIC_0       ((uint8_t)0x44U) /* 'D' */
#define DTC_MIRROR_MAGIC_1       ((uint8_t)0x4DU) /* 'M' */

/** Wire format version of the header below. Bump on any layout change. */
#define DTC_MIRROR_FORMAT_VERSION ((uint8_t)1U)

/** Fixed-position header fields: magic(2) + version(1) + count(2). */
#define DTC_MIRROR_HEADER_BYTES  (5U)

/** Per-entry size: 3-byte DTC code + 1-byte status. */
#define DTC_MIRROR_ENTRY_BYTES   (4U)

/** CRC-32 trailer appended after the last entry. */
#define DTC_MIRROR_CRC_BYTES     (4U)

/* [EDS#145] NVM_KEY_DTC_RECORDS: the first/most recent records
 * (dtc_record_t order), written and loaded with the status mirror.
 *   [magic:2 'D','R'][version:1][code:3 big-endian] x 4 [crc32:4]
 * CRC-32 over [version..last code byte]. A code of 0 is an empty record. */
#define DTC_RECORDS_MAGIC_0       ((uint8_t)0x44U) /* 'D' */
#define DTC_RECORDS_MAGIC_1       ((uint8_t)0x52U) /* 'R' */
#define DTC_RECORDS_FORMAT_VERSION ((uint8_t)1U)
#define DTC_RECORDS_BYTES         (19U)

/**
 * Maximum number of DTC status entries the mirror will ever persist to NVM
 * in a single record (issue #123).
 *
 * This is deliberately NOT UDS_MAX_DTC_COUNT (128): platform/nvm_store.h's
 * NVM_MAX_RECORD_BYTES (512) is a hard per-record cap shared with unrelated
 * NVM consumers (security counters, session stats), so widening it is out
 * of scope for this module. Instead the mirror caps what IT promises to
 * persist at the largest entry count that provably fits the existing
 * 512-byte budget alongside the 5-byte header and 4-byte CRC trailer:
 *
 *     DTC_MIRROR_HEADER_BYTES + N * DTC_MIRROR_ENTRY_BYTES + DTC_MIRROR_CRC_BYTES
 *         <= NVM_MAX_RECORD_BYTES
 *     5 + N*4 + 4 <= 512  =>  N <= 125.75  =>  N = 125
 *
 * At N=125: 5 + 125*4 + 4 = 509 bytes (fits). At N=126: 513 bytes (does not).
 * dtc_database can still register up to UDS_MAX_DTC_COUNT (128) DTCs — any
 * entries at index >= DTC_MIRROR_MAX_PERSISTED_DTCS are simply not written
 * to the NVM mirror (documented limitation, not a truncated/corrupt write).
 *
 * A build-time _Static_assert in dtc_mirror.c ties DTC_MIRROR_MAX_BYTES
 * (derived from this constant, below) to NVM_MAX_RECORD_BYTES, so any
 * future change to these sizing constants that breaks the inequality
 * fails the build instead of failing silently at runtime.
 */
#define DTC_MIRROR_MAX_PERSISTED_DTCS ((uint16_t)125U)

/** Maximum mirror payload: header + max_persisted_entries × entry_size + CRC trailer. */
#define DTC_MIRROR_MAX_BYTES     ((uint16_t)(DTC_MIRROR_HEADER_BYTES + \
                                  (DTC_MIRROR_MAX_PERSISTED_DTCS * DTC_MIRROR_ENTRY_BYTES) + \
                                  DTC_MIRROR_CRC_BYTES))

/**
 * [EDS#632] Longest time, in ms, a monitor-path change of a bit in
 * DTC_PERSIST_TRIGGER_MASK waits before dtc_mirror_tick_1ms() writes it,
 * and the shortest time between two such writes (the wear cap).
 * Override with -DDTC_MIRROR_FLUSH_INTERVAL_MS=<ms> (at least 1).
 */
#ifndef DTC_MIRROR_FLUSH_INTERVAL_MS
#define DTC_MIRROR_FLUSH_INTERVAL_MS  (1000U)
#endif

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialize the DTC mirror module.
 *
 * Must be called after nvm_store_init() and before dtc_database_init().
 * Stores internal state — does not perform the load (call dtc_mirror_load
 * after dtc_database_init() to populate status bytes).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_ALREADY_INITIALIZED if already initialized.
 */
uds_status_t dtc_mirror_init(void);

/**
 * @brief Load persisted DTC status bytes from NVM into the live database.
 *
 * Reads the NVM mirror, validates it as a whole (magic, version, declared
 * length, CRC-32), and — only if every check passes — calls
 * dtc_database_restore_status() for each entry (not gated by
 * ControlDTCSetting, EDS#448). Validation happens before any
 * entry is applied, so a corrupt record can never partially apply (issue
 * #114). Entries in a valid mirror for DTCs not registered in the current
 * database are silently skipped (e.g. after a firmware update removes a
 * DTC).
 *
 * Must be called after both nvm_store_init() and dtc_database_init()
 * (and after all DTCs have been registered via dtc_database_register()).
 *
 * Three cases are distinguished:
 *   1. No record present, OR a record that does not begin with the v1
 *      magic (DTC_MIRROR_MAGIC_0/1) — returns OK, no entries applied.
 *      DELIBERATE MIGRATION DECISION (issue #114): this bucket also
 *      covers a legacy pre-header record written by firmware built before
 *      this fix (format was [count:2][entries...], no magic). Such a
 *      record cannot be safely told apart from unrelated short/garbage
 *      data by shape alone, so it is not specially parsed — it is
 *      discarded exactly like "no mirror yet". This is a one-time,
 *      expected event on first boot after upgrading to this fix: the
 *      device boots with DTC status at defaults instead of restoring
 *      whatever the legacy record held. Everything logged/reported from
 *      here on is therefore about NEW-format records only.
 *   2. A record with valid magic, version, length and CRC — restored,
 *      returns OK.
 *   3. A record with valid magic but a version mismatch, a byte count
 *      inconsistent with its declared entry count, or a CRC-32 mismatch —
 *      returns UDS_STATUS_ERR_NVM_DATA_CORRUPT and applies nothing. This
 *      is the case the original bug silently mishandled: a corrupted
 *      NEW-format record is now reported as a distinct condition rather
 *      than partially (and silently) applied.
 *
 * @return UDS_STATUS_OK if mirror loaded (or no/legacy mirror — case 1).
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if dtc_mirror_init() not called,
 *         or NVM is not ready.
 * @return UDS_STATUS_ERR_PLATFORM if NVM read error.
 * @return UDS_STATUS_ERR_NVM_DATA_CORRUPT if a v1-tagged record fails
 *         validation (case 3 above). Note: callers written before this
 *         fix that only treat OK/ERR_NOT_INITIALIZED/ERR_PLATFORM as
 *         non-fatal will treat this new status as a hard error — see
 *         issue #114 PR discussion.
 *
 * [EDS#145] After a valid status mirror is applied, the first/most recent
 * records are restored from NVM_KEY_DTC_RECORDS. No record (first boot after
 * upgrading) leaves them empty and returns OK. A record that fails its
 * magic/version/length/CRC check applies nothing and returns
 * UDS_STATUS_ERR_NVM_DATA_CORRUPT, with the status bytes already restored.
 * The records are not restored without a valid status mirror: a record
 * naming a DTC whose status was lost would contradict 0x19 0x02.
 */
uds_status_t dtc_mirror_load(void);

/**
 * @brief Persist all current DTC status bytes to NVM.
 *
 * Serializes the entire dtc_database status array and writes it as a
 * single atomic record to NVM_KEY_DTC_MIRROR, then the first/most recent
 * records to NVM_KEY_DTC_RECORDS [EDS#145]. Returns the first failure.
 *
 * Called from:
 *   - zephyr_port_nvm_flush() (before ECU reset)
 *   - SID 0x14 (ClearDiagnosticInformation) after clearing all DTCs
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if dtc_mirror_init() not called.
 * @return UDS_STATUS_ERR_PLATFORM if NVM write failed.
 */
uds_status_t dtc_mirror_flush_all(void);

/**
 * @brief [EDS#632] Write a pending monitor-path status change to NVM within
 *        DTC_MIRROR_FLUSH_INTERVAL_MS (see WHAT IS GUARANTEED above).
 * Called by uds_server_tick_1ms() once per millisecond, so every integration
 * that ticks the server gets it, under the same lock as the tick. Does
 * nothing before dtc_mirror_init(), and nothing while no change is pending.
 */
void dtc_mirror_tick_1ms(void);

/**
 * @brief Persist a single DTC status update to NVM immediately.
 *
 * Called by SID 0x14 after a single-DTC clear.
 * Performs a full flush (atomic overwrite of the whole mirror record)
 * since NVS does not support partial record update.
 *
 * @param[in] dtc_code    The DTC code whose status changed.
 * @param[in] status_byte The new status byte value.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if not initialized.
 * @return UDS_STATUS_ERR_PLATFORM if NVM write failed.
 *
 * @note PERFORMANCE: This writes the entire mirror at once. It is for 0x14
 *       only; monitor-path changes are rate-limited through
 *       dtc_mirror_tick_1ms() instead [EDS#632].
 */
uds_status_t dtc_mirror_save_one(uint32_t dtc_code, uint8_t status_byte);

/**
 * @brief Persist the result of a clear-all to NVM.
 *
 * Called by SID 0x14 (ClearDiagnosticInformation, all groups) AFTER
 * dtc_database_clear_non_permanent() has cleared RAM. Writes the live table
 * and the (now empty) first/most recent records, so NVM matches RAM:
 * cleared DTCs as 0x50, and a permanent DTC, which the clear leaves
 * standing, with its status unchanged. [EDS#534] It used to write 0x50 for
 * every entry, permanent ones included, so a permanent DTC lost its status at
 * the next boot.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if not initialized.
 * @return UDS_STATUS_ERR_PLATFORM if NVM write failed.
 */
uds_status_t dtc_mirror_clear_all(void);

/**
 * @brief Check whether the DTC mirror module is initialized.
 *
 * @return true if dtc_mirror_init() has completed.
 * @return false otherwise.
 */
bool dtc_mirror_is_ready(void);

#ifdef UNIT_TEST
/**
 * @brief Reset DTC mirror to power-on defaults.
 *
 * FOR TEST USE ONLY. Not available in production builds.
 * [MISRA 8.7] Prototype provided here so all callers have a visible
 * declaration; guarded so the symbol is unreachable in production.
 */
void dtc_mirror_test_reset(void);
#endif /* UNIT_TEST */

#ifdef __cplusplus
}
#endif

#endif /* DTC_MIRROR_H */
