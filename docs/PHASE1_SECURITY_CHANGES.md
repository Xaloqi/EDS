# Phase 1 — Production Security Hardening

## Change Record
## Historical record. Current unit test count: 36. See TESTING_STRATEGY.md.

**Phase:** 1 (Security Hardening)  
**Baseline:** Phase 6B (post housekeeping)  
**Status:** Complete — 29/29 unit tests pass, CI-ready

---

## Summary of Changes

### 1. AES-128-CMAC + TRNG Seed/Key Algorithm (`core/uds_security_algo.*`)

Replaced the Phase 5 XOR reference stub with a production-grade algorithm:

| Item | Before (XOR stub) | After (Phase 1) |
|------|-------------------|-----------------|
| Seed length | 4 bytes | **8 bytes** (TRNG nonce 6B + seq_hi + seq_lo) |
| Key length | 4 bytes | **4 bytes** (first 4 bytes of AES-128-CMAC output) |
| Key derivation | `key[i] = seed[i] ^ 0xAA` | **AES-128-CMAC(level_key, seed)**, truncated to 4 bytes |
| Entropy source | None (deterministic) | **Hardware TRNG** via pluggable callback |
| Replay protection | None | **16-bit monotonic sequence counter** embedded in seed bytes [6:7] |
| OEM key injection | Not supported | `uds_security_algo_set_level_key()` |
| OEM algo override | Not supported | `uds_security_algo_set_derive_cb()` |

### 2. New Module: `core/uds_aes_cmac.*`

Portable, table-free AES-128 + RFC 4493 CMAC implementation:
- No dynamic memory allocation
- No lookup tables (timing-attack resistant structure)
- Verified against RFC 4493 known-answer test vectors (4/4 KATs pass)
- MISRA C:2012 alignment intended

### 3. Seed Layout Change (BREAKING — internal only)

```
Old (4 bytes):  [rand_0][rand_1][rand_2][rand_3]
New (8 bytes):  [nonce_0..5][seq_hi][seq_lo]
```

- `UDS_SECURITY_SEED_LEN` changed: 4 → **8**
- `UDS_SECURITY_KEY_LEN` unchanged: **4**
- `uds_security.c`: key-length check updated to compare against `UDS_SECURITY_KEY_LEN`, not `seed_len`

### 4. Zephyr TRNG Integration (`examples/basic_ecu/src/main.c`)

- TRNG device reference guarded by `#if DT_HAS_CHOSEN(zephyr_entropy)`
- Without overlay: LFSR fallback (development only, logged as warning)
- With overlay: Hardware TRNG registered via `uds_security_algo_set_rng_cb()`
- Enables board overlays to opt-in to TRNG without changing source code

### 5. GUI Bridge Update (`gui/server/bridge.py`)

- AES-CMAC key derivation in demo mode now matches `uds_security_algo.c`
- 8-byte seed support in the WebSocket protocol
- Python `cryptography` library used for CMAC computation

---

## CI Fixes Applied (Phase 6A→6B→Phase 1)

| ID | Fix |
|----|-----|
| B1 | `DTC_OVERLAY_FILE` added to `native_sim` CI west build command |
| B2 | `can_frame_flags_t` / `can_filter_flags_t` → `(uint8_t)0U` in `zephyr_can.c` |
| H1 | `ISOTP_SF_MAX_PAYLOAD_LEN` moved outside `#ifndef` guard |
| H2 | `GEN_SAFETY_DID_COUNT` regenerated to 5 |
| H3 | `dtc_mirror_init()` + `dtc_mirror_load()` added to `uds_generated_init()` |
| H4 | Security level comment off-by-one fixed in `did_safety_wrappers.h` |

---

## Housekeeping (M1–M5)

| ID | Action |
|----|--------|
| M1 | `a.out` removed; `*.out`, `*.o`, `build_test/` added to `.gitignore` |
| M2 | `tests/legacy_unit/` removed; `tests/unit_runnable/` is the single canonical test directory (36 modules) |
| M3 | `project/src/` shadow copies removed; `scripts/sync_shadow_copies.sh` stub created |
| M4 | Phase 3 GUI files archived to `gui/legacy/phase3-configurator/` |
| M5 | `UDS_STATUS_ERR_CONDITIONS_NOT_CORRECT` duplicate alias removed (MISRA C:2012 Rule 4.2) |

---

## Remaining Pre-Production Items

1. **Production key injection** — Replace compile-time placeholder AES keys in `uds_security_algo.c` with OTP/HSM-provisioned keys via `uds_security_algo_set_level_key()`
2. **MISRA checker integration** — Add PC-lint / Polyspace to CI (currently `-fanalyzer` only)
3. **ISO 26262 work products** — Safety manual, requirements traceability, WCET analysis
4. **Stack usage budget** — Process `-fstack-usage` output; enforce per-module limits

---

## Test Coverage

```
Unit tests:    29/29 PASS
RFC 4493 KATs: 4/4   PASS
CI jobs:       4/4   PASS (native_sim + nucleo_h743zi + unit-tests + static-analysis)
```
