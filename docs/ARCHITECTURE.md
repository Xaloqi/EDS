# Architecture — Xaloqi EDS

**Version:** v1.6.0  
**Status:** Production-ready. All CI jobs passing.

---

## 1. Overview

The Xaloqi EDS is a modular automotive diagnostics stack for Zephyr RTOS and FreeRTOS-based
ECUs. It implements a complete UDS (ISO 14229) server over two transports:

- **ISO-TP (ISO 15765-2)** over CAN — the original and primary transport
- **DoIP (ISO 13400-2)** over Ethernet/TCP — added in v1.6.0, Zephyr and FreeRTOS+LwIP bindings

Both transports use the same UDS server core, ASIL-B safety wrappers, and YAML-driven code
generation. Transport selection is a one-line change in `diagnostics_config.yaml`.

The system is structured to support:

- Deterministic embedded behavior — no dynamic allocation, no recursion
- ASIL-B safety-oriented design — 5-step mandatory validation on every DID access
- Configuration-driven diagnostics — YAML in, C code out
- Zephyr RTOS native integration — CAN drivers, threading, timers
- Scalable DID and DTC databases — static tables, compile-time bounds

---

## 2. Architectural Principles

**Layered design.** Protocol logic, transport, safety validation, and generated data tables are in
separate layers with explicit interfaces. No layer reaches across into a non-adjacent layer.

**Configuration-driven diagnostics.** Diagnostics behavior is defined in `diagnostics_config.yaml`,
not hardcoded. The code generator produces DID handlers, safety wrappers, DTC tables, and the
complete init sequence. Changing a DID means editing YAML and re-running codegen — not editing C.

**Safety-first.** Every DID access must pass a 5-step ASIL-B validation chain generated at codegen
time. The chain cannot be disabled by runtime configuration. Violations produce UDS-compliant
negative responses and increment a violation counter accessible for field diagnostics.

**No dynamic memory.** No `malloc` or `free` anywhere in the stack. All buffers are statically
allocated with compile-time bounds. CI enforces this with a `grep`-based no-malloc check.

**No recursion.** All state machines use explicit state variables. The ISO-TP state machine and UDS
session FSM are iterative, not recursive.

**Explicit error model.** Every public API function returns `uds_status_t`. No `void` return on
any safety path. Initialization guards (initialized flags) on all context structures.

---

## 3. High-Level Architecture

```
+----------------------------------------------------------+
|                    ECU Application                       |
|   (vehicle logic, sensors, actuators, DID handlers)      |
+----------------------------------------------------------+
                           │
                           ▼
+----------------------------------------------------------+
|                     UDS Server                           |
|   Service dispatcher — 14 SIDs                           |
|   Session manager (default / extended / programming)     |
|   Security manager (AES-128-CMAC, levels 1–N)            |
+----------------------------------------------------------+
                           │
                           ▼
+----------------------------------------------------------+
|                 ASIL-B Safety Validation                 |
|   5-step DID access chain (generated, non-bypassable)    |
|   uds_safety_self_test() callable at boot                |
|   Violation counters + last_violation_code               |
+----------------------------------------------------------+
                           │
                           ▼
+----------------------------------------------------------+
|               Diagnostics Databases                      |
|   DID database — read/write handlers, access rules       |
|   DTC database — status flags, severity                  |
|   NVM DTC mirror — fault history across resets           |
+----------------------------------------------------------+
                           │
                           ▼
+----------------------------------------------------------+
|                 Transport Layer                          |
|                                                          |
|   ┌─────────────────────────┐  ┌──────────────────────┐ |
|   │  ISO-TP (ISO 15765-2)   │  │  DoIP (ISO 13400-2)  │ |
|   │  SF/FF/CF/FC            │  │  TCP/IP              │ |
|   │  N_As/N_Bs/N_Cs/N_Cr   │  │  Routing Activation  │ |
|   │  STmin sub-ms range     │  │  DiagnosticMessage   │ |
|   └────────────┬────────────┘  └──────────┬───────────┘ |
+----------------------------------------------------------+
                 │                           │
                 ▼                           ▼
          CAN bus / loopback         Ethernet (TCP port 13400)
```

---

## 4. Repository Structure

```
embedded-diagnostics-suite/
│
├── core/
│   ├── uds_server.c/h          # Central UDS request handler and dispatcher
│   ├── uds_session.c/h         # Session FSM (default / extended / programming)
│   ├── uds_security.c/h        # SecurityAccess manager (AES-128-CMAC)
│   ├── uds_safety.c/h          # Safety self-test, violation counters
│   └── uds_services/
│       ├── service_0x10.c      # DiagnosticSessionControl
│       ├── service_0x11.c      # ECUReset
│       ├── service_0x14.c      # ClearDiagnosticInformation
│       ├── service_0x19.c      # ReadDTCInformation
│       ├── service_0x22.c      # ReadDataByIdentifier
│       ├── service_0x27.c      # SecurityAccess
│       ├── service_0x28.c      # CommunicationControl
│       ├── service_0x2e.c      # WriteDataByIdentifier
│       ├── service_0x31.c      # RoutineControl
│       ├── service_0x34.c      # RequestDownload
│       ├── service_0x36.c      # TransferData
│       ├── service_0x37.c      # RequestTransferExit
│       ├── service_0x3e.c      # TesterPresent
│       └── service_0x85.c      # ControlDTCSetting
│
├── transport/
│   ├── isotp.c/h               # ISO-TP state machine (SF/FF/CF/FC)
│   ├── can_transport.c/h       # CAN frame abstraction
│   ├── zephyr_can.c/h          # Zephyr CAN driver binding  [CAN path]
│   └── doip/                   # DoIP transport (v1.6.0)    [DoIP path]
│       ├── doip_server.c/h     # ISO 13400-2 ECU server — platform-agnostic core
│       ├── zephyr_lwip.c/h     # Zephyr BSD-socket binding (zsock_*)
│       └── freertos_lwip.c/h   # FreeRTOS + LwIP binding (lwip_socket)
│
├── config/
│   ├── did_database.c/h        # DID registration and lookup
│   ├── dtc_database.c/h        # DTC registration and status
│   └── dtc_mirror.c/h          # NVM DTC mirror (fault history across resets)
│
├── platform/
│   ├── platform_api.h              # Platform-neutral interface (both HALs implement this)
│   ├── uds_flash_ops.c/h           # Flash ops registration singleton (platform-independent)
│   ├── zephyr/                     # Zephyr RTOS HAL
│   │   ├── zephyr_port.c/h         # CAN init, timestamps, ECU reset, NVM flush
│   │   ├── zephyr_mutex.c/h        # k_mutex wrapper (diag_mutex_t)
│   │   ├── zephyr_timer.c/h        # k_timer + k_sem 1ms tick
│   │   ├── zephyr_wdt.c/h          # Zephyr WDT driver binding
│   │   ├── zephyr_flash_ops.c/h    # MCUboot secondary-slot flash (DFU)
│   │   ├── nvm_store.c/h           # Zephyr NVS backend (production)
│   │   ├── nvm_store_mock.c        # RAM-backed NVM stub (native_sim / host tests)
│   │   ├── platform_doip.h         # eds_doip_platform_start() declaration
│   │   └── platform_doip.c         # DoIP registration shim → zephyr_lwip.c
│   └── freertos/                   # FreeRTOS HAL
│       ├── freertos_platform_api.c # eds_platform_* — reset, NVM flush, uptime, init
│       ├── freertos_can.c/h        # can_transport_ops_t over static ISR-safe queue
│       ├── freertos_nvm.c          # nvm_store_* routing customer NVM ops
│       ├── platform_doip.h         # eds_doip_platform_start_freertos() declaration
│       └── platform_doip.c         # DoIP registration shim → freertos_lwip.c
│
├── generated/                  # !! DO NOT HAND-EDIT — overwritten by codegen !!
│   ├── did_database.c          # Generated DID registration table
│   ├── did_handlers.c/h        # Generated handler declarations
│   ├── did_safety_wrappers.c/h # Generated ASIL-B 5-step wrapper chain
│   ├── dtc_database.c          # Generated DTC registration table
│   ├── uds_init.c/h            # Generated init sequence
│   ├── config.h                # Generated compile-time constants
│   └── safety_config.h         # Generated ASIL compile-time assertions
│
├── tools/
│   ├── codegen.py              # Main generator: YAML → C/H (8 templates)
│   ├── testgen.py              # Test generator: YAML → pytest + CANoe CAPL (v1.1.0)
│   ├── config_parser.py        # YAML validation and parsing helpers
│   └── templates/              # Jinja2 templates for all generated files
│       ├── *.c.j2 / *.h.j2        C/H templates (codegen.py)
│       ├── *.py.j2                 pytest templates (testgen.py)
│       ├── ecu_diagnostics_test_suite.can.j2   CAPL master suite
│       ├── test_did_XXXX.can.j2                CAPL per-DID tests
│       └── test_dtcs.can.j2                    CAPL DTC tests
│
├── ide/
│   └── vscode-extension/           # VS Code extension (publisher: eds-diagnostics)
│       ├── src/
│       │   ├── extension.ts         # Activation, commands, status bar
│       │   ├── validator.ts         # In-process YAML validation (squiggles)
│       │   ├── hoverProvider.ts     # Key-path resolver + HoverProvider
│       │   ├── hoverDocs.ts         # Documentation for all YAML keys
│       │   └── codegenRunner.ts     # Terminal-based codegen execution
│       ├── schemas/
│       │   └── diagnostics_config.schema.json  # JSON Schema auto-applied to YAML files
│       └── package.json             # publisher: eds-diagnostics; activates on .yaml
├── gui/                        # React/TypeScript configurator + live dashboard
│   ├── src/components/
│   │   ├── ConfiguratorPanel.tsx  # YAML DID/DTC editor + codegen trigger
│   │   ├── CodegenSection.tsx     # Run Codegen button, live terminal output
│   │   └── ...                    # Dashboard panels (overview, dids, dtcs, ...)
│   ├── src/store/
│   │   ├── useDashboardStore.ts   # WebSocket state + codegen event handling
│   │   └── useConfiguratorStore.ts # YAML config state + export/import
│   └── server/bridge.py           # WebSocket bridge: ECU ↔ GUI + codegen runner
│
├── examples/
│   ├── basic_ecu/              # Minimal reference ECU (start here)
│   ├── bms_ecu/                # Battery Management System
│   ├── motor_controller/       # Motor controller with speed/torque DIDs
│   └── ardep/                  # ARDEP reference platform with DFU
│
├── tests/
│   ├── unit_runnable/          # Canonical Unity unit tests (36 modules)
│   ├── integration/            # Python ISO-TP/UDS simulation tests
│   ├── harness/                # Build harness tests (68 tests)
│   └── mocks/                  # Zephyr port mock + NVM mock for host builds
│
└── scripts/
    ├── build_tests.sh          # Runs all 36 unit test modules
    └── build_harness.sh        # Runs all 68 harness tests
```

---

## 5. UDS Server Core

### Service Dispatcher

The central entry point for all UDS requests. Receives a parsed UDS PDU from the ISO-TP layer,
looks up the SID in the dispatch table, and calls the corresponding service handler.

**14 implemented service handlers (v1.0.0):**

| SID  | Service |
|------|---------|
| 0x10 | DiagnosticSessionControl |
| 0x11 | ECUReset |
| 0x14 | ClearDiagnosticInformation |
| 0x19 | ReadDTCInformation |
| 0x22 | ReadDataByIdentifier |
| 0x27 | SecurityAccess |
| 0x28 | CommunicationControl |
| 0x2E | WriteDataByIdentifier |
| 0x31 | RoutineControl |
| 0x34 | RequestDownload |
| 0x36 | TransferData |
| 0x37 | RequestTransferExit |
| 0x3E | TesterPresent |
| 0x85 | ControlDTCSetting |

### Session Manager (`core/uds_session.c`)

Tracks the active diagnostic session. Handles session switching, P3 timeout management,
and TesterPresent keep-alive. Supported sessions: `default` (0x01), `programming` (0x02),
`extended` (0x03).

### Security Manager (`core/uds_security.c`)

Implements the SecurityAccess (0x27) seed/key protocol. Algorithm: AES-128-CMAC. Tracks
the active security level, enforces the delay timer after failed attempts, and counts
failed attempts per session. The `xor_stub` algorithm is available for development builds
only — it must not be used in production firmware.

---

## 6. Transport Layer (`transport/`)

Two transports share the same UDS server core. Transport selection is a YAML field:
`ecu.transport: can` (default) or `ecu.transport: doip`.

### 6.1 ISO-TP (ISO 15765-2) — `transport/isotp.c`

Implements ISO 15765-2 in full:

| Frame Type | Direction | Description |
|---|---|---|
| Single Frame (SF) | Rx/Tx | Payload ≤ 7 bytes — single CAN frame |
| First Frame (FF) | Rx/Tx | First segment of a multi-frame message |
| Consecutive Frame (CF) | Rx/Tx | Subsequent segments, sequence-numbered |
| Flow Control (FC) | Rx/Tx | CTS / Wait / Overflow from receiver |

All four timing parameters are implemented: N_As, N_Bs, N_Cs, N_Cr. The state machine is
iterative (no recursion). The Zephyr CAN driver binding in `platform/zephyr/zephyr_can.c` abstracts
the platform-specific CAN API.

**TX padding (`ISOTP_TX_PADDING`, default off):** When enabled, unused bytes in all transmitted
SF, FF (CAN FD escape), CF, and FC frames are padded with `ISOTP_TX_PADDING_BYTE` (default `0xCC`)
and DLC is extended to 8 (Classic CAN) or the next valid CAN FD DLC. Required for OEM testers that
expect fixed-length frames. See [docs/ISOTP_PADDING.md](ISOTP_PADDING.md).

### 6.2 DoIP (ISO 13400-2) — `transport/doip/`

Added in v1.6.0. Implements the ECU (entity) side of the DoIP diagnostics protocol over TCP.

#### DoIP Feature Matrix (v1.8.x, ISO 13400-2:2019)

| Feature | Payload type | Status | Notes |
|---|---|---|---|
| TCP server on port 13400 | — | ✅ Implemented | `eds_doip_server_run()`, blocking loop |
| DoIP header version check | — | ✅ Implemented | Version 0x02, inverse byte 0xFD validated on every frame |
| Routing Activation Request | 0x0005 | ✅ Implemented | Activation type 0x00 (Default) only |
| Routing Activation Response | 0x0006 | ✅ Implemented | Response codes 0x00 (denied), 0x10 (OK), 0x11 (already active) |
| Alive Check Request | 0x0007 | ✅ Implemented | Handled without routing activation requirement |
| Alive Check Response | 0x0008 | ✅ Implemented | Empty payload per ISO 13400-2 §9.2.7 |
| Diagnostic Message | 0x8001 | ✅ Implemented | Source/target address validation → `uds_server_process_request()` |
| Diagnostic Message Positive Ack | 0x8002 | ✅ Implemented | Sent before UDS dispatch per ISO 13400-2 §9.5 |
| Diagnostic Message Negative Ack | 0x8003 | ✅ Implemented | NACK codes: 0x03 (invalid src), 0x04 (unknown tgt), 0x05 (too large), 0x07 (not routed) |
| Single active TCP connection | — | ✅ Implemented | Sequential accept; routing state reset on new connection |
| UDP Vehicle Identification Request | 0x0001 | ❌ Not implemented | Out of scope for v1.8.x |
| UDP Vehicle Identification w/ EID | 0x0002 | ❌ Not implemented | Out of scope for v1.8.x |
| Vehicle Announcement | 0x0004 | ❌ Not implemented | Out of scope for v1.8.x |
| Entity Status Request/Response | 0x4001/0x4002 | ❌ Not implemented | Out of scope for v1.8.x |
| Activation type 0x01 (OEM-specific) | 0x0005 | ❌ Not implemented | Only Default (0x00) supported |
| Multiple simultaneous TCP clients | — | ❌ Not implemented | One client per server instance; clients queue sequentially |
| TLS transport | — | ❌ Not implemented | Plain TCP only |
| IPv6 | — | ❌ Not implemented | IPv4 only |

**Source references:** `transport/doip/doip_server.h`, `transport/doip/doip_server.c` — `doip_handle_frame()` switch statement.  
**Standard:** ISO 13400-2:2019 §9 (payload types), §9.3 (routing activation), §9.5 (diagnostic message).

**Frame format symmetry:** byte-for-byte compatible with xaloqi-tester `DoipBus`
(TestLab v1.1.0). The same `source_address=0x0E00 / target_address=0xE400 / port=13400`
defaults are used on both sides.

**Platform bindings:**

| File | Platform | API |
|---|---|---|
| `transport/doip/zephyr_lwip.c` | Zephyr | zsock_socket, zsock_poll, zsock_send, zsock_recv |
| `transport/doip/freertos_lwip.c` | FreeRTOS + LwIP | lwip_socket, lwip_select, lwip_send, lwip_recv |

Both bindings implement `eds_doip_platform_ops_t` — doip_server.c never calls
zsock_* or lwip_* directly. The same transport-agnostic core handles all protocol logic.

**YAML configuration:**

```yaml
ecu:
  transport: doip           # "can" (default), "doip", or "both"
  doip:
    logical_address: "0xE400"
    source_address:  "0x0E00"
    port:            13400
```

---

## 7. Tooling

### Wireshark Dissector (`extras/wireshark/eds.lua`)

A single-file Wireshark Lua dissector decodes all three EDS protocol layers — UDS (ISO 14229-1),
ISO-TP (ISO 15765-2), and DoIP (ISO 13400-2) — inline in packet captures taken from vcan
interfaces, real CAN buses, or Ethernet DoIP sessions. Engineers evaluating EDS on hardware can
open a `.pcap` file and read UDS service names, NRC descriptions, ISO-TP PCI frame types, and
DoIP payload type names directly in the Wireshark decode tree without post-processing hex dumps.
Installation takes three steps (copy, reload, optionally Decode As for CAN); see
`extras/wireshark/README.md`. DoIP registration is automatic on TCP and UDP port 13400. ISO-TP
on CAN requires a manual **Analyze → Decode As → eds_isotp** step because CAN arbitration IDs
are application-defined and cannot be auto-detected.

---

## 8. Diagnostics Databases (`config/`)

### DID Database

Static registration table of Data Identifiers. Each entry holds: DID identifier, data length,
read handler pointer, write handler pointer, allowed sessions bitmask, minimum security level.
Lookup is O(n) linear scan over the static table.

### DTC Database

Static registration table of Diagnostic Trouble Codes. Each entry holds: DTC code, name,
severity level, current status byte (test failed, confirmed, pending, etc.).

### NVM DTC Mirror (`config/dtc_mirror.c`)

Persists DTC fault history across ECU resets using Zephyr's NVS (Non-Volatile Storage) API.
`dtc_mirror_init()` initializes the NVS partition. `dtc_mirror_load()` restores the last known
DTC status into the in-RAM DTC database at boot. Both are called by the generated
`uds_generated_init()` sequence.

---

## 9. ASIL-B Safety Validation Layer

Every DID read or write passes through a generated 5-step chain before the handler is called.
The chain is produced by `tools/codegen.py` and cannot be bypassed by runtime configuration.

```
Step 1 — DID existence          → NRC 0x31 if DID not in database
Step 2 — Session permission     → NRC 0x7F if current session not in DID's allowed list
Step 3 — Security level         → NRC 0x33 if active level < DID's required level
Step 4 — Access permission      → NRC 0x31 if read/write not permitted for this DID
Step 5 — Data length            → NRC 0x13 if request length != DID's defined length
```

Supporting safety mechanisms:

- `uds_safety_self_test()` — callable at boot to verify the safety module is correctly
  initialised before entering the diagnostics loop
- Violation counters — incremented on each failed safety check, readable via DID
- `last_violation_code` — stores the NRC of the most recent violation for field diagnostics
- Compile-time assertions in `generated/safety_config.h` — `GEN_SAFETY_DID_COUNT` must
  match the number of DIDs in the config; CI fails if they diverge

**Requirement traceability tags:** REQ-SAFE-001 through REQ-SAFE-007 in `core/uds_safety.h`.

---

## 10. Code Generation System (`tools/`)

Diagnostics configuration is defined in `diagnostics_config.yaml`. Running `codegen.py`
produces all C/H files in `generated/`. Generated files must not be hand-edited.

### YAML → Generated file mapping

| YAML section | Generated files |
|---|---|
| `dids` | `did_database.c`, `did_handlers.c/h`, `did_safety_wrappers.c/h` |
| `dtcs` | `dtc_database.c` |
| `sessions` + `security_levels` | `config.h`, `safety_config.h` |
| all sections | `uds_init.c/h` |

### Test generation (`tools/testgen.py`)

A separate generator produces a complete test suite from the same YAML config. Two output
formats are supported, controlled by command-line flags.

**pytest output (default)** — placed in `generated/tests/`:

Each DID gets tests for valid read/write, session boundary conditions, and security access
denial. Each DTC gets tests for set, clear, and report operations. Tests run against a
built-in Python ECU simulator — no hardware or CANoe license required.

**CANoe CAPL output (`--capl` flag)** — placed in `generated/tests/capl/`:

| File | Contents |
|---|---|
| `ecu_diagnostics_test_suite.can` | Master suite: full ISO-TP layer, UDS helpers, service TCs, DID smoke TCs |
| `test_did_XXXX.can` | Per-DID exhaustive tests — one file per configured DID |
| `test_dtcs.can` | DTC service tests (0x14 ClearDTC, 0x19 ReadDTCInformation sub-functions) |
| `README_CANOE.md` | CANoe workspace import guide |

Each per-DID `.can` module generates testcases conditionally based on the DID's access policy:
positive read, echo check, response length check, wrong-session gate (NRC 0x7F), security gate
(NRC 0x33), 5-read stability, and write happy-path / wrong-length / wrong-session / security-locked
(only when the DID is writable or has session/security constraints). A `basic_ecu` with 5 DIDs and
2 DTCs produces 47 `testcase` functions across 7 `.can` files.

```bash
# pytest only:
python3 tools/testgen.py --config diagnostics_config.yaml --out generated/

# pytest + CAPL:
python3 tools/testgen.py --capl --config diagnostics_config.yaml --out generated/

# CAPL only:
python3 tools/testgen.py --capl --capl-only --config diagnostics_config.yaml --out generated/
```

---

## 11. GUI Configurator (`gui/`)

A React + TypeScript application with two modes: a **YAML configurator** for building
diagnostics configurations, and a **live dashboard** for monitoring a running ECU.

### Configurator panel (`⊕ CONFIG`)

- Edit DID definitions (id, name, length, sessions, security level, read/write handlers)
- Edit DTC definitions (code, name, severity)
- Import existing `diagnostics_config.yaml` via paste
- Export YAML — copy to clipboard or download as `diagnostics_config.yaml`
- **Run Codegen button** — sends the current YAML to `bridge.py`, which runs
  `tools/codegen.py` as a subprocess and streams stdout/stderr line-by-line back to
  the panel in a terminal-style display. On success, shows the list of generated files
  with colour-coded chips (`.c` green, `.h` blue, test files amber). On failure, shows
  the last error lines from stderr with exit code.

The codegen integration completes the Layer 2 → Layer 3 handoff without leaving the GUI:
configure DIDs → click Generate → C files appear in `generated/` → rebuild firmware.

### Live dashboard panels

Seven panels accessible from the nav bar: Overview (ECU status, session control),
Data IDs (read/write), Security (seed/key), Routines (0x31), DTCs, Raw Frames, Console.

### `server/bridge.py`

WebSocket server (`ws://localhost:8765`) that acts as a bridge between the React frontend
and the ECU. Handles all UDS commands (read DID, write DID, session change, security
access) and the `run_codegen` command. Supports two modes:

- `--mode demo` — synthetic ECU simulation, no hardware required
- `--mode can` — real CAN via python-can and SocketCAN (`vcan0` or PCAN-USB)

The `run_codegen` handler saves the YAML payload to a temp file, resolves the repo root
relative to `bridge.py`'s own path, spawns `codegen.py` as an async subprocess, and
streams each output line as a `codegen_log` WebSocket event. On completion it sends a
`codegen_result` event with success status, generated file list, and duration in ms.

---

## 11.5. VS Code Extension (`ide/vscode-extension/`)

A TypeScript VS Code extension (publisher: `eds-diagnostics`, activates on `onLanguage:yaml`)
that integrates EDS into the developer's editor.

### Features

**Inline validation (squiggles)** — the `src/validator.ts` module runs in-process on every
keystroke and produces `vscode.Diagnostic` objects for:

- Invalid DID ID format (must match `0x[0-9A-Fa-f]{4}`)
- Invalid DTC code format (must match `0x[0-9A-Fa-f]{6}`)
- Duplicate DID IDs or DTC codes within the same file
- `data_length > 64` bytes — ASIL-B `ASIL_B_MAX_DID_DATA_LEN` violation (Error)
- Invalid enum values for `sessions`, `algorithm`, DTC `severity`
- `write_security_level = 0` on a writable DID (ASIL-B Warning)
- `xor_stub` algorithm present (development-only Warning)

**Hover documentation** — `src/hoverDocs.ts` provides Markdown tooltips on every YAML key
with ISO 14229 context, allowed values, UDS wire protocol details, and example snippets.
`src/hoverProvider.ts` resolves the key path under the cursor (normalising `dids[0].id` to
`dids[].id`) and returns the correct tooltip.

**Command palette** (`Ctrl+Shift+P`):

| Command | Action |
|---|---|
| `EDS: Run Codegen` | Runs `python3 tools/codegen.py` on active YAML in a reusable terminal |
| `EDS: Run Codegen (with options)` | QuickPick for flags (`--safety-wrappers`, `--asil-level`, output dir) |
| `EDS: Validate diagnostics_config.yaml` | Forces validation and opens the Problems panel |
| `EDS: Open Documentation` | Opens `AI_CONTEXT.md` in the browser |

**Status bar** — when `diagnostics_config.yaml` is active shows `$(play) EDS: Run Codegen`,
`$(error) EDS: N errors`, or `$(warning) EDS: N warnings`. Click to run codegen.

**JSON Schema** (`schemas/diagnostics_config.schema.json`) — covers all 8 top-level blocks,
13 DID fields, 4 DTC fields, and all routine fields. Auto-applied to `diagnostics_config.yaml`
by the `yamlValidation` contribution point. Works with the Red Hat YAML extension for
autocomplete and schema-driven validation in addition to the in-process validator.

### Settings

| Setting | Default | Description |
|---|---|---|
| `eds.python3Path` | `python3` | Python interpreter path |
| `eds.codegenOutputDir` | `generated/` | Output directory for generated C files |
| `eds.codegenExtraArgs` | `["--safety-wrappers","--asil-level","B"]` | Extra codegen.py arguments |
| `eds.enableHoverDocs` | `true` | Enable hover documentation |
| `eds.enableDiagnostics` | `true` | Enable inline squiggles |
| `eds.autoRunCodegenOnSave` | `false` | Auto-run codegen when YAML is saved (skips if errors present) |

---

## 12. ECU Examples (`examples/`)

| Example | Description | DIDs | DTCs | Routines | Primary target |
|---|---|---|---|---|---|
| `basic_ecu` | Minimal reference ECU — VIN, serial, engine speed, coolant temp | 5 | 2 | 3 | native\_sim, Nucleo-H743ZI2 |
| `basic_ecu_freertos` | Same as basic\_ecu on FreeRTOS (stub CAN loopback) | 5 | 2 | 3 | QEMU Cortex-M4, any FreeRTOS MCU |
| `basic_ecu_doip` | Same as basic\_ecu served over DoIP on Zephyr | 5 | 2 | 3 | native\_sim (loopback), any Zephyr Ethernet board |
| `basic_ecu_doip_freertos` | Same as basic\_ecu served over DoIP on FreeRTOS + LwIP | 5 | 2 | 3 | Any FreeRTOS + LwIP Ethernet MCU |
| `sensor_ecu` | Zone controller with Zephyr sensor API — temperature + voltage → DTCs | 7 | 4 | 2 | native\_sim, any Zephyr sensor board |
| `safeboot_ecu` | MCUboot DFU over UDS (`safeboot.enabled: true`, `platform: zephyr`) | 5 | 3 | 2 | Nucleo-H743ZI2 (MCUboot required) |
| `safeboot_freertos_ecu` | FreeRTOS OTA DFU over UDS — STM32H743 dual-bank, no MCUboot (`platform: freertos`) | 5 | 3 | 2 | Nucleo-H743ZI2 / QEMU Cortex-M4 (CI RAM stub) |
| `robot_joint_controller_ecu` | Robot joint controller — position, velocity, torque, soft limits | 10 | 5 | 3 | native\_sim, any Zephyr CAN board |
| `bms_ecu` | Battery Management System — cell voltages, temperatures, SoC, SoH | 24 | 10 | 5 | native\_sim |
| `motor_controller_ecu` | Motor controller — speed, torque, temperatures, calibration | 27 | 8 | 6 | native\_sim |
| `ardep_ecu` | ARDEP I/O controller — PowerIO, CAN/LIN status, DFU, calibration | 35 | 19 | 6 | native\_sim, ARDEP board |

The first five examples ship with committed generated output (DID handlers, safety wrappers, uds\_init) so the public repo is immediately evaluatable without running codegen. Run `pytest` against the simulator in under 60 seconds — no CAN hardware needed.

---

## 13. CI Pipeline (`.github/workflows/ci.yml`)

16 jobs run on every push and pull request:

| Job | What it validates |
|---|---|
| `unit-tests` | All 36 Unity unit test modules (host-native, no Zephyr SDK) |
| `integration-tests` | Generated pytest suite in simulator mode |
| `firmware-integration-tests` | pytest + per-routine tests against compiled firmware |
| `static-analysis` | GCC `-fanalyzer` + MISRA cppcheck (zero errors) |
| `gui-build` | TypeScript typecheck + Vite production build |
| `zephyr-native` | `west build -b native_sim` (basic\_ecu) |
| `zephyr-stm32` | Cross-compile for Nucleo-H743ZI2 (basic\_ecu) |
| `ardep-example` | ARDEP codegen + simulator tests (35 DIDs, 19 DTCs) |
| `bms-example` | BMS codegen + simulator tests (24 DIDs, 10 DTCs) |
| `bms-zephyr-native` | `west build -b native_sim` (bms\_ecu) |
| `mc-example` | Motor controller codegen + simulator tests (27 DIDs) |
| `mc-zephyr-native` | `west build -b native_sim` (motor\_controller\_ecu) |
| `sensor-example` | SensorECU codegen + generated file check (7 DIDs, 4 DTCs) |
| `robotics-example` | Robot Joint Controller codegen + generated file check |
| `safeboot-example` | SafeBootECU: verifies `zephyr_flash_ops_init()` generated when enabled; regression check that `basic_ecu` does not generate it |
| `freertos-qemu` | FreeRTOS ARM cross-compile for QEMU Cortex-M4 (`basic_ecu_freertos`) |
| `freertos-safeboot` | FreeRTOS OTA DFU compile — QEMU Cortex-M4, RAM stub flash (`safeboot_freertos_ecu`) |
| `doip-integration` | basic\_ecu\_doip native_sim build · 24 DoIP unit tests · pytest end-to-end (skipped when xaloqi-tester absent) |

Global env: `XALOQI_LICENSE_SKIP=1` (codegen bypasses license check in CI — `_license.py` and templates are not present in the public repo).

---

## 14. Target Hardware

| Board | Use | Status |
|---|---|---|
| `native_sim` (Zephyr) | CI, development, simulation | ✅ All CI jobs pass |
| QEMU `mps2-an386` (FreeRTOS) | FreeRTOS CI build, Cortex-M4 validation | ✅ `freertos-qemu` + `freertos-safeboot` CI jobs green |
| STM32 Nucleo-H743ZI2 | Hardware validation, customer demo | ✅ Overlay written; HiL validation planned |
| FRDM-K64F | Fifth ECU example (planned) | 🔲 Planned |

---

## 15. Extensibility

The architecture supports future extensions without structural changes:

- Additional UDS services — add `service_0xNN.c` + register in dispatcher
- New DID or DTC — edit YAML, re-run codegen
- New RTOS port — implement `platform/platform_api.h` and the `nvm_store_*` API for
  the target RTOS. See `platform/freertos/` as a reference implementation.
- Additional transports — DoIP (ISO 13400-2) is shipped in v1.6.0. SOME/IP validator
  is shipped in TestLab v1.2.0. Future: SOVD bridge (OpenSOVD / Eclipse SDV).
- AUTOSAR Classic — the service layer is decoupled from the transport; an AUTOSAR PDU
  router could be substituted below the UDS server without changing service handlers
- Hardware-in-the-loop CI — planned, self-hosted runner on Nucleo

---

## 16. Design Constraints Summary

| Constraint | Enforcement |
|---|---|
| No `malloc` / `free` | CI no-malloc-check job (`grep` in CI) |
| No recursion | Code review + call-graph audit |
| All safety checks mandatory | Generated wrappers — cannot be disabled |
| Static buffer bounds | Compile-time assertions in `safety_config.h` |
| `uds_status_t` on all APIs | Code review; no `void` on safety paths |
| Zephyr v3.7+ | Pinned revision in `west.yml` |
