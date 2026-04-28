# Safety Model

## Xaloqi EDS — ASIL-B Candidate

> **ASIL Decomposition Notice**
> The component-level hazard analysis (EDS-HARA-001) identifies two hazard events
> (HE-01 unauthorised DID write, HE-02 unauthorised firmware download) with worst-case
> system-level ASIL D. Xaloqi EDS implements the diagnostic interface safety goals at
> **ASIL-B**. The residual ASIL-C/D gap is addressed at the vehicle system level by the
> integrating OEM or Tier-1, per ISO 26262-9 §5 ASIL decomposition. This is the standard
> decomposition pattern for a diagnostic component: the component is not responsible for
> the full system ASIL. See EDS-HARA-001 Section 5 (Professional tier) for the full
> decomposition argument.


| Field | Value |
|---|---|
| Safety target | ASIL-B candidate (ISO 26262-6:2018) |
| Requirements | REQ-SAFE-001 through REQ-SAFE-007, REQ-DTC-NVM-01, REQ-DTC-NVM-02 |
| Implementation | `core/uds_safety.c`, `generated/did_safety_wrappers.c` |
| Self-test | `uds_safety_self_test()` — callable at boot (ISO 26262-6 §9.4.3) |
| Last updated | 2026-04-02 |

---

## Contents

1. [Safety Objectives](#1-safety-objectives)
2. [Safety Philosophy](#2-safety-philosophy)
3. [Safety Requirements](#3-safety-requirements)
4. [Access Control Model](#4-access-control-model)
5. [ASIL-B 5-Step Validation Chain](#5-asil-b-5-step-validation-chain)
6. [Negative Response Codes](#6-negative-response-codes)
7. [Security Access (SID 0x27)](#7-security-access-sid-0x27)
8. [DTC Fault Persistence (REQ-DTC-NVM-01)](#8-dtc-fault-persistence-req-dtc-nvm-01)
9. [Generated Safety Code](#9-generated-safety-code)
10. [Safety Boundaries and Assumptions](#10-safety-boundaries-and-assumptions)
11. [MISRA C:2012 Alignment](#11-misra-c2012-alignment)
12. [ISO 26262 Work Products](#12-iso-26262-work-products)
13. [Safety Testing](#13-safety-testing)

---

## 1. Safety Objectives

The EDS safety model is designed to prevent four classes of diagnostic safety incident:

| Incident | Prevention mechanism |
|---|---|
| Unauthorised diagnostic access | Session + security level gating on every DID and routine |
| Unsafe modification of ECU state | Write operations require security unlock; ASIL-B wrapper enforced |
| Incorrect diagnostic data exposure | DID existence + data-length validation on every read |
| Misuse of diagnostics in restricted sessions | Session validation before any handler invocation |

No diagnostic data access occurs without passing the mandatory validation chain. This is enforced at code-generation time: the YAML-to-C pipeline cannot produce a DID without a corresponding safety wrapper.

---

## 2. Safety Philosophy

### Fail-Safe Behaviour

Invalid or unauthorised diagnostic requests must never produce undefined behaviour. Every failure path returns a deterministic UDS negative response (NRC). The ECU never enters an unhandled state due to a malformed diagnostic request.

### Centralised Validation

All DID and DTC data access is routed through the generated safety wrappers in `generated/did_safety_wrappers.c`. Service handlers do not call DID callbacks directly. This creates a single, auditable enforcement point for all access control rules.

### Explicit Access Control

Every DID declares its access contract in `diagnostics_config.yaml`:

- Which session is required (`min_session`)
- Which security level is required for writes (`write_security_level`)
- Whether it is read-only, write-only, or read/write (`access`)

The code generator encodes these contracts into the wrapper at generation time. Runtime enforcement requires no configuration lookup — the rules are compiled in.

### Deterministic Execution

Safety validation executes in bounded, deterministic time. No dynamic allocation, no recursion, no blocking operations anywhere in the validation path.

---

## 3. Safety Requirements

Seven ASIL-B requirements govern the diagnostics safety architecture, plus one ASIL-B DTC persistence requirement:

| ID | ASIL | Statement | Implementation |
|---|---|---|---|
| REQ-SAFE-001 | ASIL-B | All DID accesses shall be bounds-checked before execution | `uds_safety_check_bounds()` in every wrapper |
| REQ-SAFE-002 | ASIL-B | Session state shall be validated prior to service dispatch | `uds_safety_check_session()` in every wrapper |
| REQ-SAFE-003 | ASIL-B | Security level shall be checked before DID write operations | `uds_safety_check_security()` in every write wrapper |
| REQ-SAFE-004 | ASIL-B | NULL pointer dereferences shall be prevented at all entry points | `uds_safety_check_null_ptr()` at start of every wrapper |
| REQ-SAFE-005 | ASIL-B | All initialisation shall be deterministic and sequenced | `uds_generated_init()` Steps 1–7 in fixed order |
| REQ-SAFE-006 | ASIL-B | Diagnostic data_length fields shall match DID database records | Length comparison in every read/write wrapper |
| REQ-SAFE-007 | ASIL-B | Stack-local buffers shall never exceed compile-time bounds | `UDS_MAX_PAYLOAD_LEN` enforced in ISO-TP and server layers |
| REQ-DTC-NVM-01 | ASIL-B | DTC NVM mirror shall be initialised and loaded at every ECU boot | `dtc_mirror_init()` at Step 3.5, `dtc_mirror_load()` at Step 5.5 |

These requirements are traceable to test cases in the Requirements Traceability Matrix (`docs/EDS_Requirements_Traceability_Matrix.csv`).

Compile-time assertions in `generated/safety_config.h` prevent any of REQ-SAFE-001..007 from being silently disabled:

```c
_Static_assert(UDS_SAFETY_ENABLE_BOUNDS_CHECK    == 1, "REQ-SAFE-001");
_Static_assert(UDS_SAFETY_ENABLE_SESSION_CHECK   == 1, "REQ-SAFE-002");
_Static_assert(UDS_SAFETY_ENABLE_SECURITY_CHECK  == 1, "REQ-SAFE-003");
_Static_assert(UDS_SAFETY_ENABLE_NULL_CHECK      == 1, "REQ-SAFE-004");
```

---

## 4. Access Control Model

### Diagnostic Sessions

Four sessions are defined, with ascending privilege:

| Session | Value | Purpose |
|---|---|---|
| Default | 0x01 | Normal ECU operation; minimal diagnostic access |
| Programming | 0x02 | Firmware download (0x34/0x36/0x37); requires security Level 1 |
| Extended | 0x03 | Advanced diagnostics; read/write most DIDs |
| SafetySystem | 0x04 | Safety-system diagnostics (OEM-specific) |

Every DID and routine declares its minimum required session via `min_session` in YAML. Requests in a lower-privilege session are rejected with NRC 0x7F.

### Security Levels

SID 0x27 (SecurityAccess) implements a seed/key challenge-response protocol:

| Level | Sub-functions | Use case |
|---|---|---|
| Level 1 | 0x01 (requestSeed) / 0x02 (sendKey) | Supplier-level write access; DFU |
| Level 2 | 0x03 (requestSeed) / 0x04 (sendKey) | OEM-level access; safety-critical calibration |

AES-128-CMAC key derivation (RFC 4493) is used. Seed entropy is provided by the platform TRNG callback registered via `uds_security_algo_set_rng_cb()`. AES keys are injected via `uds_security_algo_set_level_key()` before production deployment — see the Security Integration Guide (Professional tier — xaloqi.com).

### Access Permissions

Each DID declares its access direction in YAML:

```yaml
access: [read]          # read-only
access: [write]         # write-only
access: [read, write]   # bidirectional
```

Attempting a write to a read-only DID returns NRC 0x31 (requestOutOfRange).

---

## 5. ASIL-B 5-Step Validation Chain

Every DID access — read or write — passes through exactly five ordered checks before the application callback is invoked:

```
  UDS Service Handler (0x22 / 0x2E)
            │
            ▼
  did_safe_read_XXXX()   or   did_safe_write_XXXX()
            │
            ├─── Step 1: NULL pointer guard ─────────────── REQ-SAFE-004
            │    uds_safety_check_null_ptr(buf, "handler")
            │    Failure → UDS_STATUS_ERR_NULL_PTR
            │
            ├─── Step 2: DID existence check ────────────── REQ-SAFE-001
            │    uds_safety_find_did(id, &entry)
            │    Failure → UDS_STATUS_ERR_DID_NOT_FOUND
            │
            ├─── Step 3: Session permission check ───────── REQ-SAFE-002
            │    uds_safety_check_session(entry, active_session)
            │    Failure → UDS_STATUS_ERR_SESSION_CONTROL
            │
            ├─── Step 4: Security level check ───────────── REQ-SAFE-003
            │    uds_safety_check_security(entry, active_sec_level)
            │    Failure → UDS_STATUS_ERR_SECURITY_ACCESS_DENIED
            │
            └─── Step 5: Data length bounds check ───────── REQ-SAFE-006
                 uds_safety_check_bounds(buf_len, entry->data_length)
                 Failure → UDS_STATUS_ERR_INVALID_PARAM
                 │
                 ▼
          Application callback invoked
```

Failure at any step stops execution immediately. The wrapper generates a UDS-compliant negative response. The application callback is never called if any check fails.

---

## 6. Negative Response Codes

All safety validation failures produce ISO 14229-1 compliant negative responses in the format `7F <SID> <NRC>`:

| Failure | NRC | ISO name |
|---|---|---|
| DID does not exist | `0x31` | requestOutOfRange |
| Wrong diagnostic session | `0x7F` | serviceNotSupportedInActiveSession |
| Security access denied | `0x33` | securityAccessDenied |
| Wrong access direction | `0x31` | requestOutOfRange |
| Data length mismatch | `0x13` | incorrectMessageLengthOrInvalidFormat |
| NULL pointer detected | `0x22` | conditionsNotCorrect |

Example: `7F 22 33` = ReadDataByIdentifier rejected, SecurityAccessDenied.

---

## 7. Security Access (SID 0x27)

The security algorithm uses AES-128-CMAC (RFC 4493 / NIST SP 800-38B):

```
seed = TRNG_8_bytes ‖ sequence_counter_2_bytes
key  = OEM_AES_128_key[level]
response = CMAC(key, seed)[0:4]   # 4-byte truncated MAC
```

The sequence counter is embedded in the seed to prevent replay attacks: a captured seed/key exchange cannot be replayed because the counter will have advanced (REQ-SAFE — see the Security Integration Guide (Professional tier — xaloqi.com) §3 for full replay protection design).

The AES implementation (`core/uds_aes_cmac.c`) is table-free and cache-timing-attack resistant — appropriate for an ECU security subsystem where shared-cache timing attacks are a realistic concern.

**Before production deployment**, placeholder keys in `core/uds_security_algo.c` must be replaced with OEM-provisioned key material via `uds_security_algo_set_level_key()`. The placeholder values are clearly marked and will be flagged by any security audit.

Lockout behaviour:

- After `UDS_SECURITY_MAX_ATTEMPTS` failed `sendKey` requests, the security module enters lockout
- Lockout duration is configurable; attempt counter is NVM-backed and survives ECU reset
- Lockout state is cleared only by expiry (not by ECU reset)

---

## 8. DTC Fault Persistence (REQ-DTC-NVM-01 / REQ-DTC-NVM-02)

DTC status bytes must survive ECU reset so that fault history is not lost when the ECU power-cycles. This is an ASIL-B requirement (REQ-DTC-NVM-01) because silently clearing fault history could mask a safety-relevant fault.

Implementation in `generated/uds_init.c`:

```c
/* Step 3.5 — DTC NVM mirror init (REQ-DTC-NVM-01) */
status = dtc_mirror_init();
/* ... soft-degrade: non-fatal if NVM not ready at first boot ... */

/* Step 5.5 — Restore DTC status bytes from NVM (REQ-DTC-NVM-01) */
uds_status_t mirror_rc = dtc_mirror_load();
```

The NVM mirror (`config/dtc_mirror.c`) serialises all registered DTC status bytes into a single Zephyr NVS record. On boot, `dtc_mirror_load()` restores those bytes into the live DTC database before any diagnostic communication begins. `dtc_mirror_flush_all()` is called from `zephyr_port_nvm_flush()` before every ECU reset (SID 0x11).

### Boot-time soft-degrade (first boot / flash erase)

If NVM is not ready at boot (e.g. first boot after flash erase), `dtc_mirror_load()` returns `UDS_STATUS_ERR_NOT_INITIALIZED`. This is treated as non-fatal — the ECU starts with all DTC status bytes cleared, which is the correct and expected behaviour after a full flash cycle. No safety-relevant fault history existed before the flash; none is lost.

### Mid-operation NVM failure (REQ-DTC-NVM-02)

If the NVM subsystem becomes permanently unavailable after a successful boot — for example, due to flash wear-out, a hardware fault, or NVS sector exhaustion — the following behaviour applies:

| Operation | NVM unavailable behaviour | Safety consequence |
|---|---|---|
| `dtc_mirror_save_one()` (status change) | Returns `UDS_STATUS_ERR_PLATFORM`; RAM status byte already updated | DTC status is current in RAM; NVM copy may be stale after next reset |
| `dtc_mirror_flush_all()` (before reset) | Returns `UDS_STATUS_ERR_PLATFORM`; ECU reset proceeds | DTC history since last successful flush is lost after reset |
| `dtc_mirror_load()` (next boot) | Loads last successfully written mirror; stale entries may be present | RAM reflects last persisted state, not state at time of NVM failure |
| Diagnostic communication (SID 0x19, 0x14) | RAM-based DTC table remains fully operational | No impact on diagnostic protocol behaviour |

**The ECU never enters an unsafe state due to NVM failure.** Diagnostic communication continues using the RAM-based DTC table. The safety consequence is limited to potential loss of DTC status history accumulated since the last successful NVM flush — which is bounded by the interval between `dtc_mirror_flush_all()` calls (i.e. between ECU resets or SID 0x14 invocations).

**Integration assumption:** The platform NVM implementation shall set `nvm_store_is_ready()` to false when the NVM subsystem is permanently degraded, so that the DTC mirror degrades gracefully rather than attempting writes to a faulty device. This assumption is documented in Section 10 (Safety Boundaries and Assumptions).

**Residual risk:** If NVM becomes unavailable without `nvm_store_is_ready()` returning false (i.e. the platform NVM driver silently discards writes), the DTC mirror will silently fail to persist status updates. This residual risk shall be mitigated by the integrator through NVM driver qualification and write verification at the platform layer.


## 9. Generated Safety Code

The safety wrapper files are produced entirely by the code generator and must not be edited manually:

| File | Content |
|---|---|
| `generated/did_safety_wrappers.c` | 5-step wrapper implementations for all DIDs |
| `generated/did_safety_wrappers.h` | Wrapper function prototypes |
| `generated/safety_config.h` | ASIL-B `_Static_assert` guards + version |

The generator guarantees:

- Every DID in the YAML has exactly one read wrapper and, if writable, one write wrapper
- The security level encoded in each write wrapper matches `write_security_level` in the YAML exactly
- `GEN_SAFETY_DID_COUNT` equals `GEN_DID_COUNT` — verified by CI at build time

`uds_safety_self_test()` in `core/uds_safety.c` exercises all seven REQ-SAFE-* check categories with known-good and known-bad inputs. It is designed to be called once at ECU startup (ISO 26262-6 §9.4.3 pre-start check guidance). If any category fails self-test, the ECU must not enter a diagnostic session.

---

## 10. Safety Boundaries and Assumptions

The EDS safety model makes the following assumptions about the integration environment. The ASIL-B properties of the stack hold only when these assumptions are satisfied:

| Assumption | Consequence of violation |
|---|---|
| Application DID/routine callbacks do not bypass the safety wrapper | Wrapper enforcement is circumvented; safety chain broken |
| The CAN transport layer correctly delivers frames in order | ISO-TP reassembly may produce corrupted PDUs |
| The platform TRNG provides adequate entropy | Seed predictability; replay attack surface opens |
| AES keys are provisioned with genuine OEM key material | Security access can be brute-forced with placeholder keys |
| `uds_generated_init()` is called before any UDS traffic is processed | Uninitialised state; undefined behaviour |
| NVM is not externally modified after `dtc_mirror_load()` | DTC status bytes become inconsistent |
| The platform NVM driver sets `nvm_store_is_ready()` to false when NVM is permanently degraded | DTC mirror silently discards writes; fault history lost without detection (residual risk — mitigate via NVM driver qualification) |

These assumptions are documented in the Safety Manual (EDS-SM-001 Rev 1.1), available to Professional tier licensees at **https://xaloqi.com**.

---

## 11. MISRA C:2012 Alignment

The production codebase (`core/`, `transport/`, `config/`, `generated/`) is analysed against MISRA C:2012 in CI via `misra_analysis.py` with cppcheck. The CI static-analysis job enforces zero open violations — any new finding requires a documented deviation entry in the MISRA Deviation Log (Professional tier — xaloqi.com) before CI will pass.

Key design choices that eliminate common MISRA violations:

- No dynamic memory allocation (`malloc`/`free` never called)
- No recursion (all state machines use explicit state variables)
- All functions return `uds_status_t`; no `void` functions on safety paths
- All inputs validated before use
- No unreachable code except deliberate `break` after `sys_reboot()` (documented as DEV-MULT-01)

---

## 12. ISO 26262 Work Products

Current status of ISO 26262-6:2018 Part 6 work products:

| Work product | Status | Location |
|---|---|---|
| Software safety requirements (ASIL-B) | Present | `EDS_Requirements_Traceability_Matrix.csv` (30 rows) — included in Professional tier safety package |
| Software architectural design | Present | `docs/ARCHITECTURE.md`, this document |
| MISRA C:2012 deviation log | Complete | the MISRA Deviation Log (Professional tier — xaloqi.com) (10 deviations, 0 open) |
| Unit testing (ISO 26262-6 Table 10) | Present | 35 modules, `tests/unit_runnable/` |
| Integration testing | Present | Firmware harness (68 tests) + generated pytest suite |
| Safety manual | Complete | EDS-SM-001 Rev 1.1 — delivered to Professional tier licensees via xaloqi.com |
| Requirements traceability matrix | Partial | REQ-SAFE-001..007 + REQ-DL-* + REQ-DTC-NVM-01 |

**Present (Professional tier):**

- HARA (EDS-HARA-001 Rev 1.0) — component-level hazard analysis, 6 hazard events, ASIL decomposition per ISO 26262-9
- Tool Qualification Argument (EDS-TQA-001 Rev 1.0) — codegen.py classified TCL1 per ISO 26262-8 §11
- Safety Manual (EDS-SM-001 Rev 1.1) — peer reviewed
- Requirements Traceability Matrix — 30 requirements, all COVERED

**Remaining gaps before formal ASIL-B claim:**

- WCET analysis on cross-compiled Cortex-M7 binary at -Os
- Independent third-party safety assessment (not required for customer evaluation)

---

## 13. Safety Testing

### Unit Tests

The following test modules specifically target safety-critical paths:

| Test file | Covers |
|---|---|
| `test_uds_safety.c` | All 7 REQ-SAFE-* categories; `uds_safety_self_test()` [TC-SAFE-LIFE-001] [REQ-SAFE-005] |
| `test_did_safety_wrappers.c` | Full 5-step chain; all NRC paths for each step |
| `test_uds_security.c` | Seed/key state machine; lockout counter; replay protection |
| `test_uds_session.c` | Session state machine; S3server timeout |
| `test_phase5_replay_protection.c` | Sequence counter; replay detection |
| `test_dtc_mirror.c` | NVM init/load/save/flush; TC-MIRROR-021/022/023 power-cycle integration |
| `test_service_0x27.c` | SecurityAccess protocol; defence-in-depth double-check |
| `test_service_0x34/36/37.c` | DFU session gate; block counter wrap; CRC-32 verify |

### Integration and Firmware Tests

- **68 harness integration tests** validate the compiled C stack including AES-CMAC key derivation, session state machine, DID safety enforcement, and RoutineControl dispatch
- **Generated simulator tests** cover every YAML-configured DID and routine with session and security gate NRCs
- **CI gate**: any open MISRA violation or failing safety test blocks merge
