// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/nvm_store_append.c
 *
 * PURPOSE: NVM store — append-only, log-structured backend for flash with
 *          large/slow erase sectors (issue #304).
 *
 * WHY THIS EXISTS:
 *   platform/zephyr/nvm_store.c hardwires Zephyr's NVS filesystem, which is
 *   architecturally unsound on STM32H7: `max-erase-time = 4000` (ms) per
 *   128 KB sector (this SoC's own devicetree binding) means a single
 *   reclamation-triggered erase can legitimately block for up to 4 seconds —
 *   40x this project's 100 ms ASIL-B watchdog window — and worse, on
 *   EVERY failed SecurityAccess attempt (core/uds_security.c writes
 *   NVM_KEY_SEC_STATE on every failure, by design, per EDS#211's
 *   lockout-bypass-by-reset fix), making the NVM write path
 *   attacker-controllable: enough bad keys eventually force a reclamation
 *   cycle. See #304's full analysis for the complete reasoning; this file
 *   is Stage 2 of the resolution path posted there.
 *
 * DESIGN — two 128 KB banks, log-structured, erase decoupled from write:
 *   - Every write APPENDS a record (never an in-place update) to the
 *     active bank: [key(2)][len(2)][payload(len)][crc32(4)], padded to the
 *     flash write-block size.
 *   - When the active bank fills, COMPACTION copies the current live set
 *     (a handful of keys, comfortably under 1 KB total — see nvm_store.h's
 *     key list) forward into the other bank, which is erased first. That
 *     erase is the only one this file ever performs, is exactly ONE
 *     bounded physical-sector erase (this hardware's own erase-block-size
 *     equals one whole bank), and is bridged across the watchdog with the
 *     same k_timer technique platform/zephyr/zephyr_flash_ops.c uses for
 *     issue #312's chunked DFU erase — scoped to exactly that one call, not
 *     a standing bypass. This converts the #304 attack from "forces a
 *     watchdog reset" (severe) to "forces a bounded multi-second response
 *     delay" (a much milder DoS, and no longer a lockout-bypass primitive:
 *     the erase can no longer interrupt itself).
 *   - Bank selection on boot: each bank's 32-byte header carries a magic
 *     value and a monotonically increasing generation counter; the bank
 *     with the higher generation (and a valid magic) is active. Neither
 *     bank valid means first-ever boot or full corruption — both banks are
 *     erased and bank 0 becomes active at generation 1.
 *   - Every record's CRC-32 is checked on read; a corrupt record is never
 *     trusted, matching this codebase's fail-closed convention elsewhere
 *     (core/uds_services/service_0x37.c's CRC check, uds_mcuboot_image.c's
 *     defensive parsing).
 *   - Delete is a native tombstone record (key | NVM_APPEND_TOMBSTONE_BIT,
 *     zero-length), not the sentinel-write heuristic nvm_store.h documents
 *     for backends without a native delete — this backend has one.
 *
 * HOST-TESTABILITY:
 *   The three raw-flash primitives (raw_read/raw_write/raw_erase) are the
 *   ONLY functions with a `#if defined(NVM_STORE_HOST_MOCK)` split; every
 *   other function — scanning, indexing, compaction, CRC, tombstones — is
 *   the SAME code under test on host and target, following this project's
 *   established pattern for new non-trivial logic (see platform/uds_sha256.c,
 *   platform/uds_mcuboot_image.c). tests/unit_runnable/test_nvm_store_append.c
 *   exercises it against the host-side RAM-backed raw_* implementation.
 *
 * SAFETY  : Security-relevant — persists SecurityAccess lockout state
 *           (EDS#211). ASIL-B candidate. Not yet formally ASIL-assessed.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "nvm_store.h"
#include "uds_types.h"
#include "uds_transfer_ctx.h"   /* uds_transfer_crc32_update/finalise — same
                                 * CRC-32 helper platform/zephyr/zephyr_flash_ops.c
                                 * already uses from platform/ code. */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(NVM_STORE_HOST_MOCK)

/** Host-test RAM-backed "flash". Sized for the test file's own bank config
 *  (small banks — the algorithm under test does not care about bank size,
 *  only that it is a multiple of NVM_APPEND_WRITE_BLOCK and >= one header
 *  plus a handful of records). */
#define NVM_APPEND_MOCK_FLASH_SIZE  (16U * 1024U)
static uint8_t s_mock_flash[NVM_APPEND_MOCK_FLASH_SIZE];

/** Host builds have no watchdog to bridge and no real write-block-size
 *  discovery — a small power-of-two exercises the padding/alignment logic
 *  without needing Zephyr headers. */
#define NVM_APPEND_WRITE_BLOCK      (8U)

#else /* real Zephyr target */

#include <zephyr/drivers/flash.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "zephyr_wdt.h"
#include "nvm_store_append.h"

/* [FIX] Matches the LOG_MODULE_REGISTER rationale already established
 * across this project's other platform/zephyr source files. */
LOG_MODULE_REGISTER(nvm_store_append, LOG_LEVEL_INF);

/** STM32H7: 32 bytes (this SoC's devicetree write-block-size). Read at
 *  runtime instead of hardcoded, so this file is not silently wrong if
 *  ever linked for a different target with a different write granularity. */
static size_t s_write_block_size;

/* --------------------------------------------------------------------------
 * [#312-style] Watchdog bridge across the one bounded erase call this file
 * ever makes. See platform/zephyr/zephyr_flash_ops.c's identical pattern
 * (s_erase_feed_timer) for the full rationale — duplicated here in
 * miniature rather than shared, since sharing it would mean this
 * storage-engine file depending on the DFU flash-ops module for an
 * unrelated reason. Worth deduplicating later if a third call site ever
 * needs the same bridge; not done now to avoid a cross-module coupling
 * this issue does not need.
 * -------------------------------------------------------------------------- */
#define NVM_APPEND_ERASE_FEED_PERIOD_MS  (20U)

static diag_wdt_t *sp_nvm_wdt = NULL;

static void s_erase_feed_timer_expiry(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    if (sp_nvm_wdt != NULL) {
        (void)diag_wdt_feed(sp_nvm_wdt);
    }
}

K_TIMER_DEFINE(s_nvm_erase_feed_timer, s_erase_feed_timer_expiry, NULL);

void nvm_store_append_set_wdt(diag_wdt_t *wdt)
{
    sp_nvm_wdt = wdt;
}

#endif /* NVM_STORE_HOST_MOCK */

/* --------------------------------------------------------------------------
 * On-flash format constants
 * -------------------------------------------------------------------------- */

/** Bank header magic ("EDSV" as a little-endian uint32_t literal). Chosen
 *  to be neither the erased pattern (0xFFFFFFFF) nor all-zero, so a blank
 *  or zero-filled region is unambiguously "no valid header here". */
#define NVM_APPEND_BANK_MAGIC       (0x45445356UL)

/** Padded header slot size — generous relative to the raw 8-byte header
 *  (magic + generation) so it is always >= any real write-block size. */
#define NVM_APPEND_HEADER_SLOT      (32U)

/** Record header: key(2) + len(2) + crc32(4). */
#define NVM_APPEND_RECORD_HDR_LEN   (8U)

/** Set on the key field to mark a tombstone (delete) record. Real keys
 *  (NVM_KEY_* in nvm_store.h) are all < 0x0010, so this can never collide
 *  with a real key's own value. */
#define NVM_APPEND_TOMBSTONE_BIT    (0x8000U)

/** Sentinel key value marking "nothing written here yet" (erased flash
 *  reads as 0xFF bytes, so an unwritten key field reads as 0xFFFF) — never
 *  a valid real key or a valid tombstone (tombstone of a real key is always
 *  <= 0x800F). Scanning stops at the first record whose key reads this. */
#define NVM_APPEND_KEY_BLANK        (0xFFFFU)

/** Maximum distinct keys this backend tracks in its in-RAM index. Real
 *  usage today is 5 live keys (nvm_store.h) plus the schema-version key —
 *  32 gives headroom without dynamic allocation. */
#define NVM_APPEND_MAX_KEYS         (32U)

/* --------------------------------------------------------------------------
 * In-RAM index — one entry per key currently live in the active bank.
 * Rebuilt by a full scan at init; maintained incrementally after that.
 * -------------------------------------------------------------------------- */

typedef struct {
    uint16_t key;        /**< Real key (tombstone bit never set here). */
    uint32_t offset;      /**< Byte offset of this record's header, relative
                            *   to the start of the active bank (i.e.
                            *   including NVM_APPEND_HEADER_SLOT). */
    uint16_t len;         /**< Payload length. */
    bool     in_use;
} nvm_index_entry_t;

/* --------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------- */

static nvm_store_cfg_t   s_cfg;
static bool               s_initialized = false;
static uint8_t             s_active_bank;      /**< 0 or 1. */
static uint32_t            s_generation;        /**< Active bank's generation. */
static uint32_t            s_write_cursor;      /**< Next free offset within
                                                  *   the active bank. */
static nvm_index_entry_t   s_index[NVM_APPEND_MAX_KEYS];

/**
 * Record-sized scratch buffers, file-scope rather than stack-local.
 *
 * nvm_store_* is single-threaded (task-context only, matching
 * nvm_store.h's documented design constraints) and never re-entrant — one
 * NVM call is always fully complete before the next begins, so a dedicated
 * static buffer per call site is safe and carries no cross-contamination
 * risk between them. This matters because a 512-byte (NVM_MAX_RECORD_BYTES)
 * buffer on the stack, in a function that can call into compaction (itself
 * needing its own such buffer), is a meaningful bite out of
 * CONFIG_DIAG_TASK_STACK_SIZE (4096 bytes by default, and this project's
 * own #31 WCET campaign measured this exact thread's real budget) — the
 * same reasoning each example's own main.c already follows for its
 * s_req_buf/s_resp_buf, for the same reason.
 */
static uint8_t s_scratch_scan_payload[NVM_MAX_RECORD_BYTES];
static uint8_t s_scratch_append_record[NVM_APPEND_RECORD_HDR_LEN + NVM_MAX_RECORD_BYTES];
static uint8_t s_scratch_compact_record[NVM_APPEND_RECORD_HDR_LEN + NVM_MAX_RECORD_BYTES];
static uint8_t s_scratch_write_compare[NVM_MAX_RECORD_BYTES];
static uint8_t s_scratch_read_payload[NVM_MAX_RECORD_BYTES];
static uint8_t s_scratch_erase_all_sec_state[NVM_MAX_RECORD_BYTES];

/* --------------------------------------------------------------------------
 * Raw flash I/O — the only host/target split in this file.
 *
 * `rel_offset` is relative to the START OF THE PARTITION (s_cfg.flash_offset
 * on Zephyr; 0 on host, where the mock buffer IS the whole partition).
 * -------------------------------------------------------------------------- */

static int raw_read(uint32_t rel_offset, void *data, size_t len)
{
#if defined(NVM_STORE_HOST_MOCK)
    if ((rel_offset + len) > (uint32_t)sizeof(s_mock_flash)) {
        return -1;
    }
    (void)memcpy(data, &s_mock_flash[rel_offset], len);
    return 0;
#else
    return flash_read((const struct device *)s_cfg.flash_dev,
                      (off_t)(s_cfg.flash_offset + rel_offset), data, len);
#endif
}

static int raw_write(uint32_t rel_offset, const void *data, size_t len)
{
#if defined(NVM_STORE_HOST_MOCK)
    if ((rel_offset + len) > (uint32_t)sizeof(s_mock_flash)) {
        return -1;
    }
    (void)memcpy(&s_mock_flash[rel_offset], data, len);
    return 0;
#else
    return flash_write((const struct device *)s_cfg.flash_dev,
                       (off_t)(s_cfg.flash_offset + rel_offset), data, len);
#endif
}

/** Erases exactly one bank (len == s_cfg.sector_size). The only place in
 *  this file a physical erase happens — bridged across the watchdog on
 *  Zephyr for this one bounded call, see the file header comment. */
static int raw_erase(uint32_t rel_offset, size_t len)
{
#if defined(NVM_STORE_HOST_MOCK)
    if ((rel_offset + len) > (uint32_t)sizeof(s_mock_flash)) {
        return -1;
    }
    (void)memset(&s_mock_flash[rel_offset], 0xFF, len);
    return 0;
#else
    int rc;

    if (sp_nvm_wdt != NULL) {
        k_timer_start(&s_nvm_erase_feed_timer,
                      K_MSEC(NVM_APPEND_ERASE_FEED_PERIOD_MS),
                      K_MSEC(NVM_APPEND_ERASE_FEED_PERIOD_MS));
    }

    rc = flash_erase((const struct device *)s_cfg.flash_dev,
                     (off_t)(s_cfg.flash_offset + rel_offset), len);

    if (sp_nvm_wdt != NULL) {
        k_timer_stop(&s_nvm_erase_feed_timer);
    }

    return rc;
#endif
}

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static uint32_t bank_base(uint8_t bank)
{
    return (uint32_t)bank * s_cfg.sector_size;
}

static size_t write_block_size(void)
{
#if defined(NVM_STORE_HOST_MOCK)
    return (size_t)NVM_APPEND_WRITE_BLOCK;
#else
    return s_write_block_size;
#endif
}

static uint32_t round_up(uint32_t value, uint32_t block)
{
    uint32_t rem = value % block;

    return (rem == 0U) ? value : (value + (block - rem));
}

static uint32_t padded_record_size(uint16_t payload_len)
{
    return round_up((uint32_t)NVM_APPEND_RECORD_HDR_LEN + (uint32_t)payload_len,
                    (uint32_t)write_block_size());
}

static void index_clear(void)
{
    (void)memset(s_index, 0, sizeof(s_index));
}

static nvm_index_entry_t *index_find(uint16_t key)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)NVM_APPEND_MAX_KEYS; i++) {
        if (s_index[i].in_use && (s_index[i].key == key)) {
            return &s_index[i];
        }
    }
    return NULL;
}

static void index_remove(uint16_t key)
{
    nvm_index_entry_t *e = index_find(key);

    if (e != NULL) {
        e->in_use = false;
    }
}

/** Returns NULL if the index is full (NVM_APPEND_MAX_KEYS distinct keys
 *  already tracked) and key is not already present — a build-time-bounded
 *  ceiling, matching this project's "no unbounded growth" constraints
 *  elsewhere. Real usage (nvm_store.h) is 6 keys; 32 is not expected to be
 *  reached in practice. */
static nvm_index_entry_t *index_upsert(uint16_t key)
{
    nvm_index_entry_t *e = index_find(key);
    uint32_t i;

    if (e != NULL) {
        return e;
    }
    for (i = 0U; i < (uint32_t)NVM_APPEND_MAX_KEYS; i++) {
        if (!s_index[i].in_use) {
            s_index[i].key    = key;
            s_index[i].in_use = true;
            return &s_index[i];
        }
    }
    return NULL;
}

/** Reads and CRC-validates a record at `rel_offset` within a bank. Returns
 *  0 on a structurally sound, CRC-valid record (key/len/is_tombstone
 *  filled in; payload NOT copied out — caller re-reads it separately when
 *  needed, keeping this scan-path allocation-free). Returns -1 on any
 *  failure — corrupt CRC, a read error, or a key of NVM_APPEND_KEY_BLANK
 *  (end of log, not an error but also not a valid record: caller checks
 *  key separately before calling this to distinguish the two). */
static int read_record_header(uint32_t bank, uint32_t rel_offset,
                              uint16_t *out_key, uint16_t *out_len,
                              bool *out_is_tombstone)
{
    uint8_t  hdr[NVM_APPEND_RECORD_HDR_LEN];
    uint16_t raw_key;
    uint16_t len;
    uint32_t stored_crc;
    uint32_t computed_crc;
    uint8_t *payload_buf = s_scratch_scan_payload;

    if (raw_read(bank_base((uint8_t)bank) + rel_offset, hdr, sizeof(hdr)) != 0) {
        return -1;
    }

    raw_key = (uint16_t)((uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8U));
    len     = (uint16_t)((uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8U));
    stored_crc = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8U) |
                ((uint32_t)hdr[6] << 16U) | ((uint32_t)hdr[7] << 24U);

    if (raw_key == (uint16_t)NVM_APPEND_KEY_BLANK) {
        return -1; /* end of log — not corruption, just "nothing here" */
    }
    if (len > (uint16_t)NVM_MAX_RECORD_BYTES) {
        return -1; /* structurally impossible — fail closed */
    }

    if (len > 0U) {
        if (raw_read(bank_base((uint8_t)bank) + rel_offset + NVM_APPEND_RECORD_HDR_LEN,
                     payload_buf, len) != 0) {
            return -1;
        }
        computed_crc = uds_transfer_crc32_update((uint32_t)0xFFFFFFFFUL, payload_buf, len);
        computed_crc = uds_transfer_crc32_finalise(computed_crc);
        if (computed_crc != stored_crc) {
            return -1; /* corrupt — never trusted, matches this project's
                        * fail-closed CRC convention elsewhere */
        }
    } else {
        /* Zero-length record: only valid shape is a tombstone. A genuine
         * zero-length DATA record is never produced by nvm_store_write()
         * (REQ: len must be >= 1), so len==0 with a non-tombstone key is
         * itself a corruption signal. */
        if ((raw_key & (uint16_t)NVM_APPEND_TOMBSTONE_BIT) == 0U) {
            return -1;
        }
    }

    *out_is_tombstone = (raw_key & (uint16_t)NVM_APPEND_TOMBSTONE_BIT) != 0U;
    *out_key = (uint16_t)(raw_key & (uint16_t)(~NVM_APPEND_TOMBSTONE_BIT));
    *out_len = len;
    return 0;
}

/** Scans `bank` from just after its header, rebuilding s_index and
 *  returning the offset of the first free (blank) position — i.e. what
 *  s_write_cursor must become. Stops at the first structurally invalid or
 *  CRC-corrupt record (treats the log as ending there — the standard
 *  log-structured-storage convention: a torn/corrupt tail record means the
 *  writer was interrupted mid-append, and this is the append-cursor
 *  recovery point). */
static uint32_t scan_bank(uint8_t bank)
{
    uint32_t offset = (uint32_t)NVM_APPEND_HEADER_SLOT;
    uint32_t bank_end = s_cfg.sector_size;

    index_clear();

    while (offset < bank_end) {
        uint16_t key;
        uint16_t len;
        bool     is_tombstone;

        if (read_record_header(bank, offset, &key, &len, &is_tombstone) != 0) {
            break; /* end of log or corrupt tail — stop here */
        }

        if (is_tombstone) {
            index_remove(key);
        } else {
            nvm_index_entry_t *e = index_upsert(key);
            if (e != NULL) {
                e->offset = offset;
                e->len    = len;
            }
            /* A full index (32 distinct keys already tracked, this one
             * new) is silently dropped rather than failing the whole
             * scan — matches this backend's fail-closed-per-record
             * philosophy without making one unexpected key fatal to
             * every other one already recovered. */
        }

        offset += padded_record_size(len);
    }

    return offset;
}

static bool read_bank_header(uint8_t bank, uint32_t *out_generation)
{
    uint8_t hdr[NVM_APPEND_HEADER_SLOT];
    uint32_t magic;

    if (raw_read(bank_base(bank), hdr, sizeof(hdr)) != 0) {
        return false;
    }

    magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8U) |
           ((uint32_t)hdr[2] << 16U) | ((uint32_t)hdr[3] << 24U);
    if (magic != (uint32_t)NVM_APPEND_BANK_MAGIC) {
        return false;
    }

    *out_generation = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8U) |
                      ((uint32_t)hdr[6] << 16U) | ((uint32_t)hdr[7] << 24U);
    return true;
}

static int write_bank_header(uint8_t bank, uint32_t generation)
{
    uint8_t hdr[NVM_APPEND_HEADER_SLOT];

    (void)memset(hdr, 0xFF, sizeof(hdr));
    hdr[0] = (uint8_t)(NVM_APPEND_BANK_MAGIC & 0xFFU);
    hdr[1] = (uint8_t)((NVM_APPEND_BANK_MAGIC >> 8U) & 0xFFU);
    hdr[2] = (uint8_t)((NVM_APPEND_BANK_MAGIC >> 16U) & 0xFFU);
    hdr[3] = (uint8_t)((NVM_APPEND_BANK_MAGIC >> 24U) & 0xFFU);
    hdr[4] = (uint8_t)(generation & 0xFFU);
    hdr[5] = (uint8_t)((generation >> 8U) & 0xFFU);
    hdr[6] = (uint8_t)((generation >> 16U) & 0xFFU);
    hdr[7] = (uint8_t)((generation >> 24U) & 0xFFU);

    return raw_write(bank_base(bank), hdr, sizeof(hdr));
}

/** Formats `bank` as a fresh, empty, active bank at `generation`: erase,
 *  then write its header. The one erase call this backend ever makes
 *  outside of compact_if_needed(). */
static uds_status_t format_bank(uint8_t bank, uint32_t generation)
{
    if (raw_erase(bank_base(bank), (size_t)s_cfg.sector_size) != 0) {
        return UDS_STATUS_ERR_PLATFORM;
    }
    if (write_bank_header(bank, generation) != 0) {
        return UDS_STATUS_ERR_PLATFORM;
    }
    return UDS_STATUS_OK;
}

/** Forward declaration — defined below append_record(), which must call it
 *  when the active bank is full. See that function's own doc comment for
 *  why it is not defined earlier instead. */
static uds_status_t nvm_store_append_compact_internal(void);

/** Appends one record (data/tombstone) to the active bank, compacting
 *  first if it would not otherwise fit. On return the record has been
 *  written and the index updated for `key`. */
static uds_status_t append_record(uint16_t key, const void *data, uint16_t len,
                                  bool is_tombstone)
{
    uint32_t needed = padded_record_size(len);
    uint8_t *buf = s_scratch_append_record;
    uint16_t wire_key = is_tombstone
                            ? (uint16_t)(key | (uint16_t)NVM_APPEND_TOMBSTONE_BIT)
                            : key;
    uint32_t crc = (uint32_t)0xFFFFFFFFUL;
    nvm_index_entry_t *e;

    if ((s_write_cursor + needed) > s_cfg.sector_size) {
        uds_status_t rc = nvm_store_append_compact_internal();
        if (rc != UDS_STATUS_OK) {
            return rc;
        }
        if ((s_write_cursor + needed) > s_cfg.sector_size) {
            /* Should not happen — the live set is always small relative
             * to a 128 KB bank — but never write past the bank. */
            return UDS_STATUS_ERR_PLATFORM;
        }
    }

    if (len > 0U) {
        crc = uds_transfer_crc32_update(crc, data, len);
    }
    crc = uds_transfer_crc32_finalise(crc);

    buf[0] = (uint8_t)(wire_key & 0xFFU);
    buf[1] = (uint8_t)((wire_key >> 8U) & 0xFFU);
    buf[2] = (uint8_t)(len & 0xFFU);
    buf[3] = (uint8_t)((len >> 8U) & 0xFFU);
    buf[4] = (uint8_t)(crc & 0xFFU);
    buf[5] = (uint8_t)((crc >> 8U) & 0xFFU);
    buf[6] = (uint8_t)((crc >> 16U) & 0xFFU);
    buf[7] = (uint8_t)((crc >> 24U) & 0xFFU);
    if (len > 0U) {
        (void)memcpy(&buf[NVM_APPEND_RECORD_HDR_LEN], data, len);
    }
    /* Pad to the write-block boundary with 0xFF (erased-flash pattern —
     * content is never reinterpreted, since scanning always advances by
     * exactly `needed` bytes computed from this record's own len). */
    if (needed > ((uint32_t)NVM_APPEND_RECORD_HDR_LEN + len)) {
        (void)memset(&buf[NVM_APPEND_RECORD_HDR_LEN + len], 0xFF,
                     needed - ((uint32_t)NVM_APPEND_RECORD_HDR_LEN + len));
    }

    if (raw_write(bank_base(s_active_bank) + s_write_cursor, buf, needed) != 0) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    if (is_tombstone) {
        index_remove(key);
    } else {
        e = index_upsert(key);
        if (e == NULL) {
            return UDS_STATUS_ERR_BUFFER_OVERFLOW; /* index exhausted */
        }
        e->offset = s_write_cursor;
        e->len    = len;
    }

    s_write_cursor += needed;
    return UDS_STATUS_OK;
}

/** Copies the current live set into the other bank (freshly erased), then
 *  makes it active. Declared here, defined below append_record() since it
 *  calls raw_read/raw_write directly rather than through append_record()
 *  (it must preserve each record's ORIGINAL bytes/CRC verbatim, not
 *  re-derive them — copying, not re-writing). */
static uds_status_t nvm_store_append_compact_internal(void)
{
    uint8_t  other_bank = (uint8_t)(1U - s_active_bank);
    uint32_t new_generation = s_generation + 1U;
    uint32_t new_cursor = (uint32_t)NVM_APPEND_HEADER_SLOT;
    uint32_t i;
    uds_status_t rc;

    rc = format_bank(other_bank, new_generation);
    if (rc != UDS_STATUS_OK) {
        return rc;
    }

    for (i = 0U; i < (uint32_t)NVM_APPEND_MAX_KEYS; i++) {
        uint8_t *rec_buf = s_scratch_compact_record;
        uint32_t rec_size;

        if (!s_index[i].in_use) {
            continue;
        }

        rec_size = padded_record_size(s_index[i].len);
        if (raw_read(bank_base(s_active_bank) + s_index[i].offset,
                     rec_buf, rec_size) != 0) {
            return UDS_STATUS_ERR_PLATFORM;
        }
        if (raw_write(bank_base(other_bank) + new_cursor, rec_buf, rec_size) != 0) {
            return UDS_STATUS_ERR_PLATFORM;
        }

        s_index[i].offset = new_cursor;
        new_cursor += rec_size;
    }

    s_active_bank  = other_bank;
    s_generation   = new_generation;
    s_write_cursor = new_cursor;
    return UDS_STATUS_OK;
}

/** Wipes the store entirely: both banks erased, bank 0 becomes active at
 *  generation 1, empty. Used for first-ever boot (neither bank has a
 *  valid header) and for schema migration (mirrors
 *  platform/zephyr/nvm_store.c's nvm_migrate_schema(), which also wipes
 *  everything including NVM_KEY_SEC_STATE on a version mismatch — a rare,
 *  deliberate firmware-upgrade event, not the routine factory-reset path
 *  nvm_store_erase_all() covers, which does preserve it). */
static uds_status_t full_wipe(void)
{
    uds_status_t rc;

    rc = format_bank(1U, 0U); /* format the INACTIVE-to-be bank first so a
                               * power-loss between the two formats never
                               * leaves both banks simultaneously invalid */
    if (rc != UDS_STATUS_OK) {
        return rc;
    }
    rc = format_bank(0U, 1U);
    if (rc != UDS_STATUS_OK) {
        return rc;
    }

    s_active_bank  = 0U;
    s_generation   = 1U;
    s_write_cursor = (uint32_t)NVM_APPEND_HEADER_SLOT;
    index_clear();
    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/** Forward declaration — nvm_store_init()'s schema check needs this before
 *  s_initialized is set; see read_internal()'s own doc comment (defined
 *  alongside nvm_store_read(), below) for why. */
static uds_status_t read_internal(uint16_t key, void *data, size_t len, size_t *out_read_len);

uds_status_t nvm_store_init(const nvm_store_cfg_t *cfg)
{
    bool     bank0_valid;
    bool     bank1_valid;
    uint32_t gen0 = 0U;
    uint32_t gen1 = 0U;
    uds_status_t rc;
    uint16_t schema_buf[1];
    size_t   schema_read_len = 0U;
    uds_status_t schema_rc;

    if (s_initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }
    if (cfg == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (cfg->flash_dev == NULL) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    if (cfg->sector_count != (uint8_t)2U) {
        /* This backend is a fixed two-bank design — see file header. */
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    s_cfg = *cfg;

#if !defined(NVM_STORE_HOST_MOCK)
    s_write_block_size = flash_get_write_block_size((const struct device *)cfg->flash_dev);
    if (s_write_block_size == 0U) {
        s_write_block_size = 32U; /* STM32H7's own known value, defensive fallback */
    }
#endif

    bank0_valid = read_bank_header(0U, &gen0);
    bank1_valid = read_bank_header(1U, &gen1);

    if (!bank0_valid && !bank1_valid) {
        rc = full_wipe();
        if (rc != UDS_STATUS_OK) {
            return rc;
        }
    } else {
        s_active_bank = (bank0_valid && (!bank1_valid || gen0 > gen1)) ? 0U : 1U;
        s_generation  = (s_active_bank == 0U) ? gen0 : gen1;
        s_write_cursor = scan_bank(s_active_bank);
    }

    /* Schema check — mirrors platform/zephyr/nvm_store.c's
     * nvm_migrate_schema(): absent or mismatched means wipe everything
     * (including NVM_KEY_SEC_STATE) and start clean.
     *
     * Uses read_internal(), NOT the public nvm_store_read() — s_initialized
     * is still false here (by design; see read_internal()'s doc comment).
     * Using the public function here was a real bug: it always observed
     * "not initialized" and always concluded "schema missing", wiping
     * NVM_KEY_SEC_STATE on every boot. */
    schema_rc = read_internal((uint16_t)NVM_KEY_SCHEMA_VERSION, schema_buf,
                              sizeof(schema_buf), &schema_read_len);
    if ((schema_rc != UDS_STATUS_OK) ||
        (schema_read_len != sizeof(schema_buf)) ||
        (schema_buf[0] != (uint16_t)NVM_SCHEMA_VERSION_CURRENT)) {
        uint16_t current = (uint16_t)NVM_SCHEMA_VERSION_CURRENT;

        rc = full_wipe();
        if (rc != UDS_STATUS_OK) {
            return rc;
        }
        rc = append_record((uint16_t)NVM_KEY_SCHEMA_VERSION, &current,
                           (uint16_t)sizeof(current), false);
        if (rc != UDS_STATUS_OK) {
            return rc;
        }
    }

    s_initialized = true;

#if !defined(NVM_STORE_HOST_MOCK)
    LOG_INF("NVM store (append): mounted bank %u, generation %u, "
            "offset 0x%08X (%u x %u B banks)",
            (unsigned)s_active_bank, (unsigned)s_generation,
            (unsigned)cfg->flash_offset, (unsigned)cfg->sector_count,
            (unsigned)cfg->sector_size);
#endif

    return UDS_STATUS_OK;
}

uds_status_t nvm_store_write(uint16_t key, const void *data, size_t len)
{
    nvm_index_entry_t *existing;

    if (data == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if ((len == (size_t)0U) || (len > (size_t)NVM_MAX_RECORD_BYTES)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    /* Skip a byte-identical re-write: avoids flash wear and, more to the
     * point for #304's threat model, avoids moving the active bank's
     * write cursor any closer to a compaction it doesn't need. */
    existing = index_find(key);
    if ((existing != NULL) && (existing->len == (uint16_t)len)) {
        uint8_t *current = s_scratch_write_compare;

        if (raw_read(bank_base(s_active_bank) + existing->offset +
                     NVM_APPEND_RECORD_HDR_LEN, current, len) == 0) {
            if (memcmp(current, data, len) == 0) {
                return UDS_STATUS_OK;
            }
        }
    }

    return append_record(key, data, (uint16_t)len, false);
}

/**
 * @brief Shared read implementation, WITHOUT the s_initialized gate.
 *
 * nvm_store_init()'s own schema check must read NVM_KEY_SCHEMA_VERSION
 * AFTER scan_bank() has populated the index but BEFORE s_initialized is
 * set true (that flag exists to gate OUTSIDE callers before init has
 * finished, not to gate init's own internal logic). Calling the public
 * nvm_store_read() from inside nvm_store_init() would always observe
 * s_initialized == false and always report NOT_INITIALIZED — which the
 * schema check would then (wrongly) treat as "no schema record yet",
 * triggering a full_wipe() on every single boot, not just the first. This
 * was a real bug caught on real hardware: NVM_KEY_SEC_STATE never survived
 * a reset because of exactly this. See issue #304.
 */
static uds_status_t read_internal(uint16_t key, void *data, size_t len, size_t *out_read_len)
{
    nvm_index_entry_t *e;
    uint8_t *payload = s_scratch_read_payload;
    size_t  copy_len;

    e = index_find(key);
    if (e == NULL) {
        return UDS_STATUS_ERR_DID_NOT_FOUND;
    }

    if (e->len > 0U) {
        if (raw_read(bank_base(s_active_bank) + e->offset + NVM_APPEND_RECORD_HDR_LEN,
                     payload, e->len) != 0) {
            return UDS_STATUS_ERR_PLATFORM;
        }
    }

    copy_len = ((size_t)e->len < len) ? (size_t)e->len : len;
    if (copy_len > 0U) {
        (void)memcpy(data, payload, copy_len);
    }
    if (out_read_len != NULL) {
        *out_read_len = (size_t)e->len;
    }

    return UDS_STATUS_OK;
}

uds_status_t nvm_store_read(uint16_t key, void *data, size_t len, size_t *out_read_len)
{
    if (data == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (len == (size_t)0U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    return read_internal(key, data, len, out_read_len);
}

uds_status_t nvm_store_delete(uint16_t key)
{
    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }
    if (index_find(key) == NULL) {
        return UDS_STATUS_OK; /* idempotent */
    }
    return append_record(key, NULL, 0U, true);
}

uds_status_t nvm_store_erase_all(void)
{
    uint8_t     *sec_state_buf = s_scratch_erase_all_sec_state;
    size_t       sec_state_len = (size_t)0U;
    bool         sec_state_present;
    uds_status_t read_rc;
    uds_status_t rc;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    /* [#280] Same fail-closed contract as platform/zephyr/nvm_store.c: only
     * a CONFIRMED absence is safe to proceed on. */
    read_rc = nvm_store_read((uint16_t)NVM_KEY_SEC_STATE, sec_state_buf,
                             sizeof(s_scratch_erase_all_sec_state), &sec_state_len);
    if (read_rc == UDS_STATUS_OK) {
        sec_state_present = true;
    } else if (read_rc == UDS_STATUS_ERR_DID_NOT_FOUND) {
        sec_state_present = false;
    } else {
        return UDS_STATUS_ERR_PLATFORM;
    }

    rc = full_wipe();
    if (rc != UDS_STATUS_OK) {
        return rc;
    }

    {
        uint16_t current = (uint16_t)NVM_SCHEMA_VERSION_CURRENT;
        rc = append_record((uint16_t)NVM_KEY_SCHEMA_VERSION, &current,
                           (uint16_t)sizeof(current), false);
        if (rc != UDS_STATUS_OK) {
            return rc;
        }
    }

    if (sec_state_present) {
        rc = append_record((uint16_t)NVM_KEY_SEC_STATE, sec_state_buf,
                           (uint16_t)sec_state_len, false);
        if (rc != UDS_STATUS_OK) {
            return rc;
        }
    }

#if !defined(NVM_STORE_HOST_MOCK)
    LOG_WRN("NVM store (append): all records erased except NVM_KEY_SEC_STATE "
            "(factory reset)");
#endif
    return UDS_STATUS_OK;
}

bool nvm_store_is_ready(void)
{
    return s_initialized;
}

#if defined(NVM_STORE_HOST_MOCK)
void nvm_mock_reset(void)
{
    (void)memset(s_mock_flash, 0xFF, sizeof(s_mock_flash));
    index_clear();
    s_initialized  = false;
    s_active_bank  = 0U;
    s_generation   = 0U;
    s_write_cursor = 0U;
}

void nvm_mock_deinit(void)
{
    s_initialized = false;
}

void nvm_mock_corrupt_byte(uint32_t byte_offset)
{
    if (byte_offset < (uint32_t)sizeof(s_mock_flash)) {
        s_mock_flash[byte_offset] ^= 0xFFU;
    }
}
#endif /* NVM_STORE_HOST_MOCK */
