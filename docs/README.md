# Xaloqi EDS
# Xaloqi Embedded Diagnostics Suite

[![CI](https://github.com/Xaloqi/EDS/actions/workflows/ci.yml/badge.svg)](https://github.com/Xaloqi/EDS/actions/workflows/ci.yml)
[![Version](https://img.shields.io/badge/version-v1.3.0-blue)](https://github.com/Xaloqi/EDS/releases/tag/v1.3.0)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](LICENSE)
[![Zephyr](https://img.shields.io/badge/Zephyr-v3.7%2B-brightgreen)](https://zephyrproject.org)

**Production-grade UDS diagnostics for Zephyr RTOS and FreeRTOS — configured in YAML, generated in seconds.**

Describe your DIDs and DTCs once. Get ISO 14229-compliant C code, ASIL-B safety wrappers, a full pytest suite, and CANoe CAPL scripts — all from a single `diagnostics_config.yaml`. No boilerplate. No hand-rolled session logic. Runs on `native_sim` in CI and on real CAN hardware the same day.

---

## The problem it solves

Building a UDS diagnostics stack from scratch on Zephyr takes 4–8 engineer-weeks: ISO-TP framing, session state machines, security access, DID dispatch, DTC persistence, ASIL-B safety wrappers, test coverage. Most teams do it once per project, inconsistently, with no reuse.

EDS replaces that with a configuration-driven workflow. Define your ECU's diagnostic interface in YAML. Run the generator. The complete C implementation drops into your Zephyr build.

---

## 60-second walkthrough

**1. Describe your ECU's diagnostic interface:**

```yaml
# diagnostics_config.yaml
ecu:
  name: "body_controller"

dids:
  - id: "0xF190"
    name: "VIN"
    data_length: 17
    access: [read]
    sessions: [default, extended]
    read_handler: "vin_read"

  - id: "0xF187"
    name: "PartNumber"
    data_length: 10
    access: [read, write]
    sessions: [extended, programming]
    read_security_level: 0
    write_security_level: 1       # requires SecurityAccess unlock
    read_handler: "part_number_read"
    write_handler: "part_number_write"

dtcs:
  - code: "0xD00101"
    name: "VoltageHigh"
    severity: high
```

**2. Generate everything:**

```bash
python3 tools/codegen.py \
  --config diagnostics_config.yaml \
  --out    generated/ \
  --safety-wrappers \
  --asil-level B \
  --test-gen
```

**3. Build and run:**

```bash
# Simulator (CI, no hardware)
west build -b native_sim examples/basic_ecu \
  -- -DDTC_OVERLAY_FILE=boards/native_sim.overlay
west build -t run

# STM32 Nucleo-H743ZI2
west build -b nucleo_h743zi2 examples/basic_ecu
west flash
```

The generator produces: DID handler stubs, ASIL-B safety wrappers, DTC registration tables, a complete UDS init sequence, a pytest suite for every DID and DTC, and optionally CANoe CAPL scripts. Regenerate any time the YAML changes — output is deterministic and CI-verifiable.

---

## What you get

| Capability | Detail |
|---|---|
| **UDS stack** | 14 services: 0x10 0x11 0x14 0x19 0x22 0x27 0x28 0x2E 0x31 0x34 0x36 0x37 0x3E 0x85 · SID 0x19 sub-functions: 0x01 0x02 0x04 0x06 0x0A (0x0B and 0x19 return NRC 0x12 — planned) |
| **ISO-TP transport** | SF / FF / CF / FC · N_As / N_Bs / N_Cr timing · STmin sub-ms range |
| **ASIL-B safety chain** | 5-step DID validation enforced at codegen time — cannot be bypassed at runtime |
| **Security** | AES-128-CMAC seed/key · TRNG-backed · configurable per-session levels · lockout with NVM persistence |
| **DTC persistence** | NVM mirror survives power cycles · 0x14 ClearDTC · 0x19 ReadDTCInformation |
| **Code generation** | YAML → 14 C/H/py templates · CLI + React GUI · reproducible deterministic output |
| **Test generation** | YAML → pytest suite per DID and DTC · simulator mode (no hardware) · firmware harness mode |
| **CANoe CAPL** | YAML → `.can` scripts for CANoe import · per-DID, per-DTC, core services |
| **VS Code extension** | Inline YAML validation · hover docs · one-click codegen |
| **MCP server** | `tools/mcp_server.py` — exposes `generate_did_config`, `run_codegen`, `validate_asil_b`, `explain_uds_error` to Claude, Cursor, and any MCP host. Included with Developer and Professional licenses. |
| **ECU examples** | basic, BMS, motor controller, ARDEP, sensor, safeboot, robot joint controller — 5–35 DIDs each, Zephyr and FreeRTOS |
| **CI pipeline** | 18-job GitHub Actions · codegen · unit tests · harness tests · Zephyr builds · FreeRTOS ARM builds · MISRA analysis · MCP server tests |

**Safety properties verified by CI on every commit:**
- Zero dynamic memory allocation (`malloc`/`free` grep gate)
- `uds_safety_self_test()` present and abort-guarded in every generated init file (ISO 26262-6 §9.4.3)
- `ASIL_B_REQUIRE_WRITE_SECURITY = True` — write-capable DIDs without a security gate are a fatal codegen error
- `GEN_SAFETY_DID_COUNT` in generated headers matches YAML ground truth

---

## Quick start

### Prerequisites

```bash
pip install west
west init -m https://github.com/Xaloqi/EDS --mr v1.1.0 eds-workspace
cd eds-workspace && west update
pip install -r tools/requirements.txt
```

### Run the basic ECU example

```bash
# Generate code from the bundled example config
python3 tools/codegen.py \
  --config examples/basic_ecu/diagnostics_config.yaml \
  --out    examples/basic_ecu/generated/ \
  --safety-wrappers --asil-level B --test-gen

# Build and run in simulator
west build -b native_sim examples/basic_ecu \
  -- -DDTC_OVERLAY_FILE=boards/native_sim.overlay
west build -t run
```

### Run the FreeRTOS example (QEMU ARM Cortex-M4)

```bash
# Clone FreeRTOS kernel
git clone --depth=1 https://github.com/FreeRTOS/FreeRTOS-Kernel.git /opt/freertos-kernel

# Generate code (same YAML as basic_ecu)
python3 tools/codegen.py \
  --config examples/basic_ecu_freertos/diagnostics_config.yaml \
  --out    examples/basic_ecu_freertos/generated/ \
  --safety-wrappers --asil-level B --no-manifest

# Build for QEMU ARM Cortex-M4
cmake -B build_freertos \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/arm-none-eabi.cmake \
  -DEDS_PLATFORM=freertos \
  -DFREERTOS_DIR=/opt/freertos-kernel \
  -DBOARD=qemu_cortex_m4 \
  -GNinja \
  examples/basic_ecu_freertos
ninja -C build_freertos
```

For the full FreeRTOS integration guide (callbacks, NVM, reset, production porting), see [`docs/INTEGRATION_GUIDE.md`](docs/INTEGRATION_GUIDE.md#freertos-integration).

### Run the test suite

```bash
# 35 Unity unit tests (host-native, no Zephyr SDK needed)
bash build_tests.sh

# 68 harness integration tests
bash build_harness.sh

# Generated pytest suite (simulator mode)
cd examples/basic_ecu/generated/tests && pytest test_services.py test_did_*.py -v --can-interface=simulator
```

### GUI configurator (optional)

```bash
cd gui && npm ci && npm run dev
# → http://localhost:5173
# Live dashboard + YAML configurator + one-click codegen
```

![Xaloqi EDS GUI — live dashboard showing DIDs panel with sensor readings, DTC panel with active faults, Security Access walkthrough, Routines panel, and Raw Frames ISO-TP log](docs/assets/gui_dashboard.svg)

*Eight panels: DIDs (read/write with live sparklines) · DTCs (active faults + status byte) · Security (seed/key exchange) · Routines (start/stop/results) · Raw Frames (ISO-TP log) · Console · Configurator · Overview. Demo mode runs without hardware: `bash gui/start-demo.sh`.*

---

## Architecture

```
diagnostics_config.yaml
        │
        ▼  python3 tools/codegen.py
┌─────────────────────────────────────────┐
│  Generated C (ASIL-B)                   │
│  did_handlers.c  did_safety_wrappers.c  │
│  uds_init.c      safety_config.h        │
└────────────────┬────────────────────────┘
                 │
┌────────────────▼────────────────────────┐
│  UDS Server Core  (core/)               │
│  14 service handlers · session FSM      │
│  security manager · ASIL-B dispatcher   │
└────────────────┬────────────────────────┘
                 │
┌────────────────▼────────────────────────┐
│  ISO-TP Transport  (transport/)         │
│  SF/FF/CF/FC · full timing · STmin      │
│  Zephyr CAN driver binding              │
└────────────────┬────────────────────────┘
                 │
           CAN bus / loopback
```

**No dynamic memory. No recursion. Static buffers only.**  
Every DID access passes a 5-step ASIL-B chain generated from YAML:

```
Step 1  DID exists?              → NRC 0x31 requestOutOfRange
Step 2  Session allows it?       → NRC 0x7F serviceNotSupportedInActiveSession
Step 3  Security level met?      → NRC 0x33 securityAccessDenied
Step 4  Access type permitted?   → NRC 0x6F conditionsNotCorrect
Step 5  Data length correct?     → NRC 0x13 incorrectMessageLengthOrInvalidFormat
```

| Directory | Contents |
|---|---|
| `core/` | UDS server, session manager, security manager, service handlers |
| `transport/` | ISO-TP state machine, Zephyr CAN driver binding |
| `config/` | DID database, DTC database, NVM mirror |
| `platform/` | Platform abstraction layer — `platform/zephyr/` (Zephyr HAL) · `platform/freertos/` (FreeRTOS HAL) · `platform_api.h` (shared interface) |
| `tools/` | `codegen.py`, `testgen.py`, 17 Jinja2 templates |
| `ide/vscode-extension/` | YAML validation, hover docs, Run Codegen command |
| `examples/` | basic\_ecu · basic\_ecu\_freertos · sensor\_ecu · safeboot\_ecu · robot\_joint\_controller\_ecu · bms\_ecu · motor\_controller\_ecu · ardep\_ecu · each with its own `generated/` subfolder |
| `gui/` | React/TypeScript configurator + live dashboard |
| `tests/` | 36 Unity unit tests, harness, Python integration tests |

---

## ECU examples

| Example | DIDs | DTCs | Routines | Boards |
|---|---|---|---|---|
| `basic_ecu` | 5 | 2 | 3 | native\_sim, Nucleo-H743ZI2 |
| `basic_ecu_freertos` | 5 | 2 | 3 | QEMU Cortex-M4, any FreeRTOS MCU |
| `sensor_ecu` | 7 | 4 | 2 | native\_sim, any Zephyr sensor board |
| `safeboot_ecu` | 5 | 3 | 2 | Nucleo-H743ZI2 (MCUboot required) |
| `robot_joint_controller_ecu` | 10 | 5 | 3 | native\_sim, any Zephyr CAN board |
| `bms_ecu` | 24 | 10 | 5 | native\_sim |
| `motor_controller_ecu` | 27 | 8 | 6 | native\_sim |
| `ardep_ecu` | 35 | 19 | 6 | native\_sim |

Each example ships with a complete generated test suite. Run `pytest` against the simulator in under 60 seconds, no CAN hardware needed.

---

## Safety and compliance

EDS targets ASIL-B readiness. The following work products are included:

| Work product | Status |
|---|---|
| Safety Manual (EDS-SM-001) | Rev 1.1 — peer reviewed |
| Requirements Traceability Matrix | 14 rows, REQ-SAFE-001–007 + DFU/DTC/flash |
| MISRA C:2012 deviation log | Complete — zero open violations |
| ASIL-B 5-step wrapper chain | Generated, CI-verified on every codegen run |
| `uds_safety_self_test()` | Present in every generated init sequence |
| WCET analysis | Host x86-64 figures available; Cortex-M7 figures pending HiL |

**AES key placeholder notice:** `core/uds_security_algo.c` ships with placeholder AES-128 keys. A compile-time gate (`CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY`) and a runtime guard in the generated init sequence prevent accidental deployment. See the Security Integration Guide (Professional tier — xaloqi.com) for the OEM key injection procedure.

---

## Licensing

| Tier | License | Price | Includes |
|---|---|---|---|
| **Community** | GPL v2 | Free | Runtime stack + examples |
| **Developer** | Commercial | €690 / year | Codegen + testgen + all examples + integration guide |
| **Professional** | Commercial | €1,990 / year | Developer + Safety Manual + MISRA log + RTM |
| **Company** | Commercial | Pricing available upon request contact@xaloqi.com | Professional company license |

The runtime stack (`core/`, `transport/`, `config/`, `platform/`) is GPL v2.  
ECU examples (`examples/`) are Apache 2.0.  
Code generation tools and Safety Manual require a commercial license.

→ **[Commercial licenses](https://xaloqi.com)**

---

## Documentation

- [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md) — zero-to-running in 15 minutes
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — full module map and design decisions
- [the Security Notice — seed entropy requirements](SECURITY_NOTICE.md) — TRNG requirements, all-zero seed rejection, LFSR fallback behaviour. Full OEM key injection and HSM offload guide is included with the Professional tier (xaloqi.com).
- [`docs/Safety_Model.md`](docs/Safety_Model.md) — ASIL-B architecture, REQ-SAFE-* traceability
- [`docs/TESTING_STRATEGY.md`](docs/TESTING_STRATEGY.md) — test layers, coverage targets, HiL plan
- [`docs/AI_CONTEXT.md`](docs/AI_CONTEXT.md) — MCP server setup, tool reference, Claude/Cursor integration guide
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — contribution guide
- [`SECURITY.md`](SECURITY.md) — vulnerability disclosure policy

---

## Requirements

- **Zephyr RTOS**: v3.7+, West 1.2+, CMake ≥ 3.20
- **FreeRTOS**: FreeRTOS-Kernel (any recent release), `arm-none-eabi-gcc` + `libnewlib-arm-none-eabi`, CMake ≥ 3.20
- Python 3.9+ with `pyyaml`, `jinja2`, `pytest`
- Node.js 18+ (GUI only)
