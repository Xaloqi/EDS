# Testing Strategy — Xaloqi EDS

**Version:** v1.3.0  
**Status:** 36/36 unit tests passing. 68/68 harness tests passing. 18/18 CI jobs green. FreeRTOS, SafeBoot, and sensor examples have generated test suites.

---

## 1. Overview

EDS uses a four-layer testing strategy: unit tests, harness tests, integration tests, and system
tests. All four layers run automatically in CI on every push and pull request.

**Current test counts (v1.3.0):**

| Layer | Count | Framework | Status |
|---|---|---|---|
| Unit tests | 35 modules | Unity (C) | ✅ All passing |
| Harness tests | 68 tests | Shell + GCC | ✅ All passing |
| Integration tests | Per-DID/DTC suite | pytest (Python) | ✅ All passing |
| System tests | native_sim E2E | Zephyr + pytest | ✅ All passing |
| Generated pytest suite | Per-DID + per-DTC | testgen.py → pytest | ✅ Generated from YAML — all 7 examples |
| Generated CANoe CAPL | Per-DID + DTC + services | testgen.py → `.can` files | ✅ Generated from YAML |
| FreeRTOS build | QEMU ARM Cortex-M4 | CMake + QEMU | ✅ basic_ecu_freertos CI green |
| SensorECU FreeRTOS build | QEMU ARM Cortex-M4 | CMake + QEMU | ✅ sensor_ecu_freertos CI green |

---

## 2. Testing Goals

The test suite verifies:

- Correct UDS protocol behaviour across all 14 implemented service handlers
- Correct ISO-TP transport: SF/FF/CF/FC framing, timing, multi-frame reassembly
- Enforcement of the ASIL-B 5-step DID access safety chain
- Correct diagnostics code generation from YAML (all 8 templates)
- Correct test generation from YAML (`testgen.py`) — verified for all 7 examples
- NVM DTC mirror persistence across simulated resets
- Zero dynamic memory allocation anywhere in the stack
- Reliable Zephyr RTOS integration on `native_sim` and `nucleo_h743zi2`
- Correct CANoe CAPL test generation from YAML (all three `.can.j2` templates)
- FreeRTOS platform HAL compiles and runs the UDS stack on QEMU ARM Cortex-M4
- SafeBoot codegen: `safeboot.enabled: true` generates `zephyr_flash_ops_init()` correctly; `false` does not regress

The suite must detect: protocol violations, security access bypasses, session handling errors,
buffer overflows, incorrect NRC responses, and configuration generation errors.

---

## 3. Test Architecture

```
System Tests (native_sim E2E — Zephyr + pytest)
              │
              ▼
Integration Tests (Python ISO-TP/UDS simulation)
              │
              ▼
Harness Tests (68 — GCC build + run on host)
              │
              ▼
Unit Tests (35 modules — Unity on host)
```

Each layer depends on the layer below it passing. CI runs all four layers in sequence.

---

## 4. Unit Tests

### Framework

Unity Test Framework (C). Tests run on the host with GCC — no Zephyr build required.
`tests/mocks/ztest_shim.h` maps Zephyr `ztest` macros (`ZTEST`, `ZTEST_SUITE`, `zassert_*`)
to Unity equivalents, so the same test files can also run on Zephyr hardware via native
`ztest` without modification. This is the correct dual-target strategy.

`tests/mocks/zephyr_port_mock.c` provides the minimal platform abstraction needed for host
builds. The `NVM_STORE_HOST_MOCK` flag activates `nvm_store_mock.c` for RAM-backed NVM
simulation, isolating tests from Zephyr's flash driver.

### Canonical test location

```
tests/unit_runnable/
```

> **Note:** `tests/unit/` also exists and is referenced by `tests/CMakeLists.txt` for the Zephyr
> native `ztest` build path. The canonical source for the 36-module CI run is `tests/unit_runnable/`,
> executed by `scripts/build_tests.sh`. Consolidation of these two directories is tracked as a
> future clean-up task.

### Running unit tests

```bash
bash scripts/build_tests.sh
# Expected: 36 tests, 0 failures
```

### Coverage — 36 unit test modules

**UDS Core (4 modules)**

| Module | Key test scenarios |
|---|---|
| `test_uds_server.c` | Request dispatch, unknown SID → NRC 0x11, buffer bounds |
| `test_uds_session.c` | Session switching, P3 timeout, TesterPresent keep-alive |
| `test_uds_security.c` | AES-128-CMAC seed/key, failed attempt counter, delay timer |
| `test_uds_safety.c` | `uds_safety_self_test()`, violation counter increment, `last_violation_code` |

**UDS Services (14 modules — one per SID)**

| Module | Key test scenarios |
|---|---|
| `test_service_0x10.c` | Session switch to all three sessions, invalid sub-function |
| `test_service_0x11.c` | Hard reset, soft reset, reset in wrong session |
| `test_service_0x14.c` | Clear all DTCs, clear by group, clear with none active |
| `test_service_0x19.c` | Report DTCs by status mask, report DTC count, report specific DTC |
| `test_service_0x22.c` | Valid DID read, unknown DID → NRC 0x31, wrong session → NRC 0x7F |
| `test_service_0x27.c` | Seed request, valid key → granted, invalid key → NRC 0x35 |
| `test_service_0x28.c` | Enable/disable Rx, enable/disable Tx, wrong session |
| `test_service_0x2e.c` | Valid DID write, write without security → NRC 0x33, wrong length → NRC 0x13 |
| `test_service_0x31.c` | Routine start, stop, request result, unknown routine ID |
| `test_service_0x34.c` | Download request in programming session, rejected in default |
| `test_service_0x36.c` | Valid block transfer, wrong block sequence counter → NRC 0x73 |
| `test_service_0x37.c` | Transfer exit, exit without prior download |
| `test_service_0x3d.c` | File transfer request, unsupported mode |
| `test_service_0x3e.c` | Tester present with/without response, suppress positive response bit |

**Transport Layer (2 modules)**

| Module | Key test scenarios |
|---|---|
| `test_isotp.c` | SF Rx/Tx, FF+CF multi-frame, FC CTS/Wait/Overflow, N_Cr timeout |
| `test_can_transport.c` | Frame queuing, filter setup, loopback round-trip |

**Diagnostics Databases (2 modules)**

| Module | Key test scenarios |
|---|---|
| `test_did_database.c` | DID lookup hit/miss, handler pointer validity, session bitmask |
| `test_dtc_database.c` | DTC status set/clear, status persistence via NVM mock, severity |

**Safety Wrappers (1 module)**

| Module | Key test scenarios |
|---|---|
| `test_did_safety_wrappers.c` | All 5 steps exercised individually; each produces the correct NRC |

**Phase-specific test modules (12 modules in `unit_runnable/`)**

Additional tests added in Phases 2–5 covering: suppress-bit handling, STmin boundary
conditions, session transition matrix, NVM DTC persistence, security algorithm correctness,
DID access table validation, and replay-protection logic.

---

## 5. Harness Tests

68 build-and-run tests exercising the compiled stack against specific input sequences.
Run by `scripts/build_harness.sh` using GCC on the host.

```bash
bash scripts/build_harness.sh
# Expected: 68 tests, 0 failures
```

Harness tests cover scenarios that are difficult to isolate in pure unit tests: multi-module
interaction, end-to-end NRC generation, and the full ISO-TP → UDS → safety wrapper →
handler call chain.

---

## 6. Integration Tests

Python tests using `pytest` that simulate a real diagnostic tester sending ISO-TP framed
UDS requests to a running `native_sim` ECU process.

```
tests/integration/
├── test_isotp_transport_flow.py
├── test_uds_read_did_flow.py
├── test_uds_write_did_flow.py
├── test_security_access_flow.py
├── test_diagnostic_session_flow.py
├── test_dtc_report_flow.py
└── test_codegen_output.py
```

```bash
# Requires native_sim ECU running in another terminal
pytest tests/integration/ -v
```

### Example flow — ReadDataByIdentifier

```
Tester → ECU  :  22 F1 90                        (read DID 0xF190)
ECU    → Tester:  62 F1 90 31 48 47 42 48 34 ...  (positive response, 17-byte VIN)
```

The test verifies: correct SID echo (0x62), correct DID echo (0xF190), response length == 17,
and that the safety chain executed without a violation.

### Example flow — SecurityAccess denial

```
Tester → ECU  :  2E F1 87 <10 bytes>    (write DID 0xF187 without security unlock)
ECU    → Tester:  7F 2E 33              (NRC 0x33 — securityAccessDenied)
```

---

## 7. Code Generation Tests

`tools/testgen.py` generates test suites directly from `diagnostics_config.yaml`. Two output
formats are supported.

### 7.1 pytest output (default)

```bash
python3 tools/testgen.py \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/

cd examples/basic_ecu/generated/tests && pytest . -v
```

Generated test coverage per DID:
- `Read_HappyPath` — positive response, correct SID echo
- `Read_ResponseEcho` — DID bytes echoed at positions [1],[2]
- `Read_ResponseLen` — response length = `data_length + 3` bytes
- `Read_WrongSession` → NRC 0x7F (only generated if `min_session > default`)
- `Read_SecurityLocked` → NRC 0x33 (only generated if `read_security_level > 0`)
- `Read_MultipleTimes` — 5 consecutive reads, consistent length (always generated)
- `Write_HappyPath` — positive write + readback (only generated if write access)
- `Write_WrongLength` → NRC 0x13 (only generated if write access)
- `Write_WrongSession` → NRC 0x7F (only generated if write access + session constraint)
- `Write_SecurityLocked` → NRC 0x33 (only generated if `write_security_level > 0`)

Generated test coverage per DTC:
- Set DTC active → confirm status byte
- Clear DTC → confirm cleared
- `ReadDTCInformation` response includes the DTC

### 7.2 CANoe CAPL output (`--capl` flag)

```bash
# Generate CAPL + pytest:
python3 tools/testgen.py --capl \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/

# Generate CAPL only:
python3 tools/testgen.py --capl --capl-only \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/
```

Output in `examples/basic_ecu/generated/tests/capl/`:

| File | Contents |
|---|---|
| `ecu_diagnostics_test_suite.can` | ISO-TP layer, UDS helpers, all service testcases, DID smoke tests, `maintest` |
| `test_did_XXXX.can` | Per-DID exhaustive testcases (conditionally generated per access policy) |
| `test_dtcs.can` | 7 DTC testcases: ClearDTC, RDTCI sub-fns 0x01/0x02/0x06/0x0A, invalid sub-fn |
| `README_CANOE.md` | CANoe workspace import guide, CAN addressing, security key instructions |

**Import into CANoe:** File → New → Test Setup → Add CAPL Test Module → select `ecu_diagnostics_test_suite.can`, then add per-DID and DTC modules. The master module provides the full ISO-TP transport layer (SF/FF/CF/FC, 0x78 response-pending loop) so no external CAPL libraries are required.

**Security key derivation:** The generated CAPL uses a key derivation stub. For `aes128_cmac` ECUs, implement via CAPL DLL or replace the stub with your HSM call. For `xor_stub` ECUs (dev/CI), the stub is correct as-is and matches the Python simulator.

**Scale:** `basic_ecu` (5 DIDs, 2 DTCs) → 8 `.can` files, 47 `testcase` functions, 8 `testgroup` functions.

---

## 8. System Tests

End-to-end tests running the complete Zephyr firmware on `native_sim`. These validate the
full stack from Zephyr thread scheduling through ISO-TP framing down to DID handler response.

```bash
# Build and run native_sim in background
west build -b native_sim examples/basic_ecu \
    -- -DDTC_OVERLAY_FILE=boards/native_sim.overlay
west build -t run &

# Run system test suite against the running process
pytest tests/integration/ -v --system
```

System test scenarios:

- ECU startup: `uds_generated_init()` completes, DTC mirror loaded from NVM
- Session switching: default → extended → programming → default
- Security unlock: full AES-128-CMAC seed/key exchange
- Multi-DID read: single 0x22 request with multiple DID IDs
- Multi-frame response: DID read triggering FF + CF ISO-TP segmentation
- DTC fault simulation: set DTC active, ReadDTCInformation, ClearDiagnosticInformation

---

## 9. CI Pipeline

All test layers run automatically in GitHub Actions on every push and pull request.

```
push / PR
   │
   ├── unit-tests              36 Unity modules via build_tests.sh
   ├── integration-tests       Generated pytest suite, simulator mode
   ├── firmware-integration    68 harness tests + firmware pytest (real C stack, no Zephyr)
   ├── ardep-example           ARDEP ECU codegen + build verification
   ├── bms-example             BMS ECU codegen + generated test verification
   ├── static-analysis         GCC -fanalyzer
   ├── gui-build               React/TypeScript build + type check
   ├── zephyr-native           Full Zephyr build, native_sim
   ├── zephyr-stm32            Cross-compile for STM32 Nucleo-H743ZI2
   ├── bms-zephyr-native       BMS example Zephyr build
   ├── mc-example              Motor controller codegen + build
   ├── sensor-example          Sensor ECU codegen + safeboot/sensor codegen checks
   ├── robotics-example        Robot joint controller codegen + build
   ├── safeboot-example        SafeBoot codegen: verifies zephyr_flash_ops_init() generated
   ├── freertos-qemu           FreeRTOS build — QEMU ARM Cortex-M4
   ├── mc-zephyr-native        Motor controller Zephyr native_sim build
   └── sensor-freertos-qemu    SensorECU FreeRTOS build — QEMU ARM Cortex-M4
```

All 17 jobs must pass before a PR can be merged.

### FreeRTOS CI job (`freertos-qemu`)

Added in v1.3.0. Builds `examples/basic_ecu_freertos` with `-DEDS_PLATFORM=freertos`
targeting QEMU ARM Cortex-M4. Downloads FreeRTOS-Kernel from GitHub, runs codegen,
builds the ELF, and verifies it exists. The same 36 unit tests run against the FreeRTOS
platform HAL (they mock the platform layer and are platform-independent).

### SafeBoot CI job (`safeboot-example`)

Added in v1.3.0. Runs codegen on `examples/safeboot_ecu/diagnostics_config.yaml`
(with `safeboot.enabled: true`) and asserts that `zephyr_flash_ops_init()` appears in
the generated `uds_init.c`. Also verifies that `examples/basic_ecu` (with `safeboot.enabled`
absent/false) does not produce that call — regression guard for the default path.

### SensorECU FreeRTOS CI job (`sensor-freertos-qemu`)

Added in v1.4.0 (Week 2). Builds `examples/sensor_ecu_freertos` with `-DEDS_PLATFORM=freertos`
targeting QEMU ARM Cortex-M4. Uses the identical `diagnostics_config.yaml` as the Zephyr
`sensor_ecu` example. Verifies codegen output, asserts the generated DID count matches the
YAML, builds the ELF (includes sensor monitor task + DID handlers), and checks binary size.

---

## 10. Coverage Targets

| Module | Target | Rationale |
|---|---|---|
| Safety wrappers | 100% | ASIL-B requirement — every step must be exercised |
| UDS Core | 90% | All service handlers + session/security FSMs |
| ISO-TP transport | 85% | All frame types + timeout paths |
| Diagnostics databases | 90% | All lookup paths, hit and miss |
| Generated code | 100% | Verified by testgen output tests |
| Platform abstraction | 70% | Mock-based; hardware paths tested by HiL (planned) |

Coverage measurement via `gcov` is enabled by the `-fprofile-arcs -ftest-coverage` CMake
flags in the unit test build. Coverage reports are generated as a CI artifact.

---

## 11. Fault Injection Testing

The stack must be robust against malformed input. Fault injection scenarios are covered
across unit, harness, and integration layers:

- Malformed UDS requests (wrong SID, truncated payload, extra bytes)
- Incorrect ISO-TP frames (wrong sequence counter, FC with Overflow status)
- Buffer overflow attempts (request length > static buffer size)
- Invalid session transitions (programming → default without reset)
- Security bypass attempts (send key without prior seed request)
- DID access in wrong session (all 14 services × 3 sessions)
- Rapid repeated SecurityAccess failures (verify lockout delay enforced)

---

## 12. Planned Testing Enhancements

| Enhancement | Phase | Status |
|---|---|---|
| Hardware-in-the-loop (HiL) on Nucleo-H743ZI2 | Phase 10 D2 | Blocked on hardware arrival |
| HiL CI job (self-hosted runner) | Phase 10 D4 | Blocked on D2 |
| `testgen-capl` CI job — render all CAPL templates and verify no `TemplateError` | v1.2 | Planned |
| Fuzz testing of ISO-TP state machine | Future | Not yet scheduled |
| WCET measurement from `-fstack-usage` output | Future | Stack usage files generated; analysis script not yet written |
| MISRA C:2012 checker in CI | Future | PC-lint or Polyspace required |
| Formal requirements traceability (REQ-SAFE-* to test IDs) | Future | Part of Safety Manual work product |
