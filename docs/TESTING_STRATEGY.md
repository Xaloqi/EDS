# Testing Strategy — Xaloqi EDS

**Version:** v1.8.2  
**Status:** 39/39 unit test modules passing. 68/68 harness tests passing. 8/8 CI jobs green. FreeRTOS, SafeBoot (Zephyr + FreeRTOS), DoIP, and sensor examples all covered.

---

## 1. Overview

EDS uses a four-layer testing strategy: unit tests, harness tests, integration tests, and system
tests. All four layers run automatically in CI on every push and pull request.

**Current test counts (v1.8.2):**

| Layer | Count | Framework | Status |
|---|---|---|---|
| Unit tests | 39 modules | Unity (C) | ✅ All passing |
| Harness tests | 68 tests | Shell + GCC | ✅ All passing |
| Integration tests | Per-DID/DTC suite | pytest (Python) | ✅ All passing |
| System tests | native_sim E2E | Zephyr + pytest | ✅ All passing |
| DoIP unit tests | 24 tests (1 module) | Unity (C) — ZTEST suite | ✅ All passing |
| DoIP integration tests | 10 tests | pytest + xaloqi-tester DoipBus | ✅ Passing (skipped when TestLab absent) |
| Generated pytest suite | Per-DID + per-DTC | testgen.py → pytest | ✅ Generated from YAML — all examples |
| Generated CANoe CAPL | Per-DID + DTC + services | testgen.py → `.can` files | ✅ Generated from YAML |
| FreeRTOS build | QEMU ARM Cortex-M4 | CMake + QEMU | ✅ basic_ecu_freertos CI green |
| SensorECU FreeRTOS build | QEMU ARM Cortex-M4 | CMake + QEMU | ✅ sensor_ecu_freertos CI green |
| DoIP FreeRTOS build | QEMU ARM Cortex-M4 | CMake + QEMU | ✅ basic_ecu_doip_freertos CI green |
| SafeBoot FreeRTOS build | QEMU ARM Cortex-M4 (RAM stub flash) | CMake + QEMU | ✅ safeboot_freertos_ecu CI green (`freertos-safeboot` job) |

---

## 2. Testing Goals

The test suite verifies:

- Correct UDS protocol behaviour across all 14 implemented service handlers
- Correct ISO-TP transport: SF/FF/CF/FC framing, timing, multi-frame reassembly
- Correct DoIP transport: header encode/decode, routing activation, diagnostic message dispatch, negative acknowledgement generation, alive check (v1.6.0)
- Enforcement of the ASIL-B 5-step DID access safety chain
- Correct diagnostics code generation from YAML (all 14 templates)
- Correct test generation from YAML (`testgen.py`) — verified for all examples
- NVM DTC mirror persistence across simulated resets
- Zero dynamic memory allocation anywhere in the stack
- Reliable Zephyr RTOS integration on `native_sim` and `nucleo_h743zi2`
- Correct CANoe CAPL test generation from YAML (all three `.can.j2` templates)
- FreeRTOS platform HAL compiles and runs the UDS stack on QEMU ARM Cortex-M4
- SafeBoot codegen: `safeboot.enabled: true` generates `zephyr_flash_ops_init()` correctly; `false` does not regress
- DoIP codegen: `ecu.transport: doip` generates `EDS_DOIP_ONLY_BUILD`-guarded `uds_init.c/.h`; CAN-only configs remain unaffected (v1.6.0)
- `EDS_DOIP_ONLY_BUILD` compile guards are present in all generated `uds_init.c/.h` files and compile cleanly under both DoIP-only and CAN-only build configurations

The suite must detect: protocol violations, security access bypasses, session handling errors,
buffer overflows, incorrect NRC responses, and configuration generation errors.

---

## 3. Test Architecture

```
DoIP Integration Tests (pytest + xaloqi-tester DoipBus — native_sim loopback)
              │
System Tests (native_sim E2E — Zephyr + pytest)
              │
              ▼
Integration Tests (Python ISO-TP/UDS simulation)
              │
              ▼
Harness Tests (68 — GCC build + run on host)
              │
              ▼
Unit Tests (39 modules — Unity on host)
```

Each layer depends on the layer below it passing. CI runs all layers in sequence.

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
> native `ztest` build path. The canonical source for the 39-module CI run is `tests/unit_runnable/`,
> executed by `scripts/build_tests.sh`. Consolidation of these two directories is tracked as a
> future clean-up task.

### Running unit tests

```bash
bash scripts/build_tests.sh
# Expected: 39 tests, 0 failures
```

### Coverage — 39 unit test modules

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
| `test_service_0x35.c` | Upload request: valid readback, read_cb=NULL → NRC 0x22, address range, direction set |
| `test_service_0x36.c` | Valid block transfer (download + upload), wrong block sequence counter → NRC 0x73 |
| `test_service_0x37.c` | Transfer exit, exit without prior download |
| `test_service_0x3d.c` | File transfer request, unsupported mode |
| `test_service_0x3e.c` | Tester present with/without response, suppress positive response bit |

**Transport Layer (3 modules)**

| Module | Key test scenarios |
|---|---|
| `test_isotp.c` | SF Rx/Tx, FF+CF multi-frame, FC CTS/Wait/Overflow, N_Cr timeout |
| `test_can_transport.c` | Frame queuing, filter setup, loopback round-trip |
| `test_doip_server.c` | 24 tests — see DoIP section below |

**Diagnostics Databases (2 modules)**

| Module | Key test scenarios |
|---|---|
| `test_did_database.c` | DID lookup hit/miss, handler pointer validity, session bitmask |
| `test_dtc_database.c` | DTC status set/clear, status persistence via NVM mock, severity |

**Safety Wrappers (1 module)**

| Module | Key test scenarios |
|---|---|
| `test_did_safety_wrappers.c` | All 5 steps exercised individually; each produces the correct NRC |

**Phase-specific test modules (13 modules in `unit_runnable/`)**

Additional tests added in Phases 2–6 covering: suppress-bit handling, STmin boundary
conditions, session transition matrix, NVM DTC persistence, security algorithm correctness,
DID access table validation, replay-protection logic, and DoIP protocol specifics.

---

## 5. DoIP Unit Tests (`test_doip_server.c`)

Added in v1.6.0. 24 tests covering the `transport/doip/doip_server.c` module on the host
via the ZTEST shim (same build path as all other unit test modules).

```bash
bash scripts/build_tests.sh
# test_doip_server is included in the standard 39-module run
```

**Test coverage — 24 ZTEST cases:**

| Test | What it verifies |
|---|---|
| `test_doip_encode_header_valid` | Header byte layout: sync byte, inverse, payload type, length |
| `test_doip_encode_header_length_field` | Length field set correctly for varying payload sizes |
| `test_doip_parse_header_valid` | Parse round-trip — encode then parse returns identical fields |
| `test_doip_parse_header_version_mismatch` | Wrong sync byte → `DOIP_NACK_INCORRECT_PATTERN` |
| `test_doip_parse_header_inv_byte_corrupt` | Inverse byte wrong → NACK |
| `test_doip_header_too_short_rejected` | Fewer than 8 bytes → length error |
| `test_doip_routing_activation_accepted` | Valid Routing Activation → positive response, connection registered |
| `test_doip_routing_activation_wrong_type` | Unsupported activation type → NACK |
| `test_doip_routing_activation_source_addr` | Source address stored, echoed in RoutingActivationResponse |
| `test_doip_alive_check_request` | AliveCheckRequest → AliveCheckResponse with correct logical addr |
| `test_doip_alive_check_response_payload` | Response payload length = 2, logical address = ECU addr |
| `test_doip_diagnostic_message_dispatch` | Valid DiagnosticMessage → `uds_server_process_request()` called |
| `test_doip_diagnostic_positive_ack` | After dispatch → DiagnosticMessagePositiveAck sent |
| `test_doip_diagnostic_negative_ack_no_route` | DiagnosticMessage before Routing Activation → NACK 0x02 |
| `test_doip_diagnostic_target_addr_mismatch` | Wrong target address → NACK 0x03 |
| `test_doip_diagnostic_source_addr_mismatch` | Source address not matching registered addr → NACK |
| `test_doip_nack_unknown_payload_type` | Unknown payload type → Generic NACK |
| `test_doip_nack_message_too_large` | Payload length exceeding buffer → NACK 0x01 |
| `test_doip_null_ops_rejected` | `eds_doip_server_init(NULL, ...)` → non-OK status |
| `test_doip_null_ctx_rejected` | NULL ctx pointer → non-OK status |
| `test_doip_boundary_min_header` | Exactly 8 bytes (header only, zero payload) — accepted |
| `test_doip_boundary_max_payload` | Max configured payload length — accepted |
| `test_doip_boundary_max_payload_plus_one` | Max + 1 byte → NACK 0x01 |
| `test_doip_reinit_rejected` | Second `eds_doip_server_init()` call → `ERR_ALREADY_INITIALIZED` |

---

## 6. Harness Tests

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

## 7. Integration Tests

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

### Example flow — SecurityAccess denial

```
Tester → ECU  :  2E F1 87 <10 bytes>    (write DID 0xF187 without security unlock)
ECU    → Tester:  7F 2E 33              (NRC 0x33 — securityAccessDenied)
```

---

## 8. DoIP Integration Tests

Added in v1.6.0. 10 pytest tests in `tests/test_doip_integration.py`. They launch the
`basic_ecu_doip` native_sim binary and exercise the ECU over TCP using the
`xaloqi-tester` `DoipBus` transport (Xaloqi TestLab). Tests are automatically skipped
(exit code 5 — treated as success in CI) when TestLab is not installed.

```bash
# Requires basic_ecu_doip native_sim binary built and xaloqi-tester installed
pytest tests/test_doip_integration.py -v --timeout=60
```

**Test coverage:**

| Test | Scenario |
|---|---|
| `test_doip_routing_activation` | Full TCP connect → Routing Activation → positive response |
| `test_doip_read_did_vin` | ReadDataByIdentifier 0xF190 over DoIP — positive response, correct length |
| `test_doip_read_did_serial` | ReadDataByIdentifier 0xF18C over DoIP |
| `test_doip_read_unknown_did` | 0x22 with unknown DID → NRC 0x31 over DoIP |
| `test_doip_session_switch` | DiagnosticSessionControl to extended session over DoIP |
| `test_doip_tester_present` | TesterPresent (suppress bit) — no response, no error |
| `test_doip_security_access_seed` | 0x27 01 → seed response, correct length |
| `test_doip_security_access_denied` | SendKey without RequestSeed → NRC 0x24 |
| `test_doip_read_dtc_information` | 0x19 01 with no active DTCs — empty response |
| `test_doip_concurrent_requests` | Two sequential requests — ECU responds to both without hang |

Environment variables consumed by the test module:

```bash
DOIP_ECU_BINARY        # path to native_sim zephyr.exe (default: build/zephyr/zephyr.exe)
DOIP_ECU_HOST          # ECU IP (default: 127.0.0.1)
DOIP_ECU_PORT          # DoIP port (default: 13400)
DOIP_STARTUP_DELAY_S   # wait before connecting (default: 2.0)
DOIP_TEST_TIMEOUT_S    # per-request timeout (default: 10.0)
```

---

## 9. Code Generation Tests

`tools/testgen.py` generates test suites directly from `diagnostics_config.yaml`. Two output
formats are supported.

### 9.1 pytest output (default)

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

### 9.2 CANoe CAPL output (`--capl` flag)

```bash
# Generate CAPL + pytest:
python3 tools/testgen.py --capl \
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

**Import into CANoe:** File → New → Test Setup → Add CAPL Test Module → select `ecu_diagnostics_test_suite.can`, then add per-DID and DTC modules. The master module provides the full ISO-TP transport layer so no external CAPL libraries are required.

**Scale:** `basic_ecu` (5 DIDs, 2 DTCs) → 8 `.can` files, 47 `testcase` functions, 8 `testgroup` functions.

---

## 10. System Tests

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

## 11. CI Pipeline

All test layers run automatically in GitHub Actions on every push and pull request.

```
push / PR
   │
   ├── unit-tests          39 Unity modules via build_tests.sh
   │                       + ASIL-B assertion checks (self-test, key gate, write security)
   │
   ├── integration-tests   Generated pytest suite, simulator mode
   │                       + zero dynamic allocation grep gate
   │
   ├── static-analysis     GCC -fanalyzer
   │
   ├── zephyr-native       Full Zephyr build, native_sim (basic_ecu)
   │
   ├── zephyr-stm32        Cross-compile for STM32 Nucleo-H743ZI2
   │
   ├── freertos-qemu       FreeRTOS build — QEMU ARM Cortex-M4 (basic_ecu_freertos)
   │
   ├── freertos-safeboot   FreeRTOS OTA DFU compile — QEMU Cortex-M4 (safeboot_freertos_ecu, RAM stub flash)
   │
   └── doip-integration    basic_ecu_doip native_sim build
                           + 24 DoIP unit tests (smoke check via build_tests.sh)
                           + 10 pytest end-to-end tests (skipped when TestLab absent)
```

All 8 jobs must pass before a PR can be merged.

### FreeRTOS CI job (`freertos-qemu`)

Added in v1.3.0. Builds `examples/basic_ecu_freertos` with `-DEDS_PLATFORM=freertos`
targeting QEMU ARM Cortex-M4. Downloads FreeRTOS-Kernel from GitHub, runs codegen,
builds the ELF, and verifies it exists. The same 39 unit tests run against the FreeRTOS
platform HAL (they mock the platform layer and are platform-independent).

### SafeBoot CI job (within `unit-tests`)

Added in v1.3.0. Asserts that `zephyr_flash_ops_init()` appears in the generated
`uds_init.c` when `safeboot.enabled: true`. Also verifies that `examples/basic_ecu`
(with `safeboot.enabled` absent/false) does not produce that call — regression guard for
the default path.

### SensorECU FreeRTOS CI job (within `freertos-qemu`)

Added in v1.4.0. Builds `examples/sensor_ecu_freertos` with `-DEDS_PLATFORM=freertos`
targeting QEMU ARM Cortex-M4. Verifies codegen output, asserts the generated DID count
matches the YAML, builds the ELF, and checks binary size.

### FreeRTOS Safeboot CI job (`freertos-safeboot`)

Added in v1.8.0. Compile-only build of `examples/safeboot_freertos_ecu/` targeting
QEMU Cortex-M4 with the RAM stub flash backend (`freertos_flash_ops.c` — no STM32
HAL required). Verifies all 49 translation units compile and `eds_safeboot_freertos.elf`
is produced. Real hardware (STM32H743ZI) path is covered by customer integration.

### DoIP integration CI job (`doip-integration`)

Added in v1.6.0. Three stages:

1. **Build:** `west build -b native_sim examples/basic_ecu_doip` with
   `-DEDS_DOIP_ONLY_BUILD` and `-DDIAG_SKIP_CODEGEN=ON` (pre-generated files committed).
   `-DDIAG_SKIP_CODEGEN=ON` is used because the Jinja2 templates live in the private
   EDS-toolchain repo and are not available in public CI. The committed generated files
   are ground truth and match template output byte-for-byte.

2. **Unit test smoke check:** `bash build_tests.sh` filtered for DoIP tests — verifies
   the 24 `test_doip_server.c` cases pass.

3. **pytest integration:** `tests/test_doip_integration.py` — 10 end-to-end tests over
   TCP loopback using `xaloqi-tester` DoipBus. Exit code 5 (all skipped — xaloqi-tester
   not installed) is treated as success in public CI. Full test run executes in private
   CI where TestLab is present.

---

## 12. Coverage Targets

| Module | Target | Rationale |
|---|---|---|
| Safety wrappers | 100% | ASIL-B requirement — every step must be exercised |
| UDS Core | 90% | All service handlers + session/security FSMs |
| ISO-TP transport | 85% | All frame types + timeout paths |
| DoIP transport | 80% | Header encode/decode, dispatch, NACK paths |
| Diagnostics databases | 90% | All lookup paths, hit and miss |
| Generated code | 100% | Verified by testgen output tests |
| Platform abstraction | 70% | Mock-based; hardware paths tested by HiL (planned) |

Coverage measurement via `gcov` is enabled by the `-fprofile-arcs -ftest-coverage` CMake
flags in the unit test build. Coverage reports are generated as a CI artifact.

---

## 13. Fault Injection Testing

The stack must be robust against malformed input. Fault injection scenarios are covered
across unit, harness, and integration layers:

- Malformed UDS requests (wrong SID, truncated payload, extra bytes)
- Incorrect ISO-TP frames (wrong sequence counter, FC with Overflow status)
- Malformed DoIP frames (wrong sync byte, corrupt inverse byte, payload too large, unknown payload type)
- Buffer overflow attempts (request length > static buffer size)
- Invalid session transitions (programming → default without reset)
- Security bypass attempts (send key without prior seed request)
- DID access in wrong session (all 16 services × 3 sessions)
- Rapid repeated SecurityAccess failures (verify lockout delay enforced)
- DoIP DiagnosticMessage before Routing Activation (→ NACK 0x02)

---

## 14. Planned Testing Enhancements

| Enhancement | Phase | Status |
|---|---|---|
| Hardware-in-the-loop (HiL) on Nucleo-H743ZI2 | Phase 10 D2 | Blocked on hardware arrival |
| HiL CI job (self-hosted runner) | Phase 10 D4 | Blocked on D2 |
| DoIP integration tests with full TestLab in private CI | v1.7.0 | Planned |
| `testgen-capl` CI job — render all CAPL templates and verify no `TemplateError` | v1.2 | Planned |
| Fuzz testing of ISO-TP and DoIP state machines | Future | Not yet scheduled |
| WCET measurement from `-fstack-usage` output | Future | Stack usage files generated; analysis script not yet written |
| MISRA C:2012 checker in CI | Future | PC-lint or Polyspace required |
| Formal requirements traceability (REQ-SAFE-* to test IDs) | Future | Part of Safety Manual work product |
