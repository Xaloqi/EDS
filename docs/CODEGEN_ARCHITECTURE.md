# Code Generation Architecture

## Xaloqi EDS — Zephyr RTOS

| Field | Value |
|---|---|
| Generator | `tools/codegen.py` |
| Template engine | Jinja2 |
| Configuration format | YAML (`diagnostics_config.yaml`) |
| Output languages | C (embedded stack), TypeScript (GUI catalog), Python (pytest suite) |
| Last updated | 2026-04-15 |

---

## Contents

1. [Overview](#1-overview)
2. [Design Goals](#2-design-goals)
3. [Pipeline](#3-pipeline)
4. [Generator Components](#4-generator-components)
5. [Configuration Format](#5-configuration-format)
6. [Template Catalogue](#6-template-catalogue)
7. [Generated C Files](#7-generated-c-files)
8. [Generated Test Files](#8-generated-test-files)
9. [Generated GUI Catalog](#9-generated-gui-catalog)
10. [CLI Reference](#10-cli-reference)
11. [Build System Integration](#11-build-system-integration)
12. [Validation and Error Handling](#12-validation-and-error-handling)
13. [Regeneration Workflow](#13-regeneration-workflow)
14. [Generated Code Constraints](#14-generated-code-constraints)

---

## 1. Overview

The Xaloqi EDS uses a configuration-driven architecture where all diagnostic behaviour is defined in a single YAML file and automatically transformed into production C code, a pytest test suite, and a TypeScript GUI catalog.

This approach delivers several compounding advantages:

- **No manual boilerplate** — every DID, DTC, and routine produces handlers, wrappers, tests, and GUI entries automatically
- **Consistent safety enforcement** — no DID can be added without a corresponding ASIL-B safety wrapper
- **Single source of truth** — changing a DID's session or security requirement in YAML propagates to C, tests, and dashboard in one codegen run
- **Scalability** — the ARDEP example configures 35 DIDs and produces 42 test files from a single 200-line YAML; without codegen this would require days of manual work

---

## 2. Design Goals

### Deterministic Output

Generated code is fully deterministic: the same YAML always produces byte-identical output. This is essential for reproducible CI builds and for diff-based code review.

### Safety Integration

The generator automatically wraps every DID with the ASIL-B 5-step validation chain. It enforces ASIL-B constraints at generation time:

- Write-capable DIDs must declare `write_security_level > 0`
- DID data length must not exceed `ASIL_B_MAX_DID_DATA_LEN` (64 bytes)
- Duplicate DID IDs are a fatal validation error
- `GEN_SAFETY_DID_COUNT` in `safety_config.h` must equal `GEN_DID_COUNT` — validated by CI

### Minimal Runtime Overhead

All configuration decisions are made at generation time. Generated C code contains only static tables, direct function mappings, and no dynamic allocation. Runtime code paths are shorter because the generator has already resolved session and security requirements into direct comparisons.

### Human-Readable Output

Generated files are intentionally readable and well-commented. A developer can open `generated/did_safety_wrappers.c` and immediately understand what each wrapper does and which YAML entry produced it. This matters during code review and safety audits.

---

## 3. Pipeline

```
diagnostics_config.yaml
         │
         ▼
  tools/codegen.py
         │
         ├─── config_parser.py        Load + normalise YAML
         │
         ├─── Validation Layer        Schema, ASIL-B constraints,
         │                            duplicate IDs, security rules
         │
         ├─── Jinja2 Templates ───── tools/templates/*.j2
         │
         ├─── C Sources ──────────── generated/*.c / *.h
         │
         ├─── Test Suite ─────────── generated/tests/*.py
         │                           (--test-gen flag)
         │
         └─── GUI Catalog ─────────── gui/src/generated/catalog.ts
                                      (--gui-types flag)
```

---

## 4. Generator Components

### Configuration Parser (`tools/config_parser.py`)

Loads the YAML file and normalises the structure into typed Python objects consumed by all downstream stages. Handles:

- Default value injection (timing parameters, CAN IDs, session defaults)
- Hex string normalisation (`0xF190` → `"0xF190"`, integer representations)
- C identifier generation from symbolic names (spaces → underscores, reserved word avoidance)

### Code Generator (`tools/codegen.py`)

The main orchestrator. Responsibilities:

1. Call `config_parser.py` to load the YAML
2. Run the validation layer (schema + ASIL-B constraints)
3. Build enriched data structures for each template
4. Render all Jinja2 templates in order
5. Write output files (only overwriting if content changed, for incremental builds)
6. Optionally call the test generator and GUI catalog generator

### Test Generator (`tools/testgen.py`)

Produces the pytest test suite when `--test-gen` is active. One test file per DID, one per routine, plus service-level and firmware harness test files. The generated tests cover:

- Happy-path read and write
- Session gate (NRC 0x7F for wrong session)
- Security gate (NRC 0x33 for insufficient security level)
- Data length mismatch (NRC 0x13)
- Invalid DID (NRC 0x31)
- RoutineControl: start, stop (if supported), requestResults (if supported), session gate, security gate

### Template System (`tools/templates/`)

14 Jinja2 templates produce all generated output. Templates have access to the full enriched configuration object and use Jinja2 filters, loops, and conditionals to produce well-structured, commented C and Python output.

---

## 5. Configuration Format

A complete `diagnostics_config.yaml` structure:

```yaml
metadata:
  ecu_name:   "BasicECU"
  version:    "1.1.0"

can:
  rx_can_id:  "0x7DF"    # ISO 15765-4 functional address
  tx_can_id:  "0x7E8"    # ECU physical response address

timing:
  p2_server_max_ms:      25
  p2_star_server_max_ms: 5000
  s3_server_timeout_ms:  5000

# Optional — SafeBoot MCUboot DFU integration (Xaloqi EDS Professional)
# Set enabled: true to generate zephyr_flash_ops_init() in uds_init.c.
# Without this block (or with enabled: false), any 0x34 RequestDownload
# is rejected with NRC 0x22 — intentionally safe.
safeboot:
  enabled: true
  platform: zephyr          # zephyr (v1.3.0); freertos support planned
  max_block_length: 256     # bytes per TransferData block

dids:
  - id:                  "0xF190"
    name:                "VehicleIdentificationNumber"
    data_length:         17
    data_type:           ascii
    min_session:         extended
    read_security_level: 0           # 0 = no unlock required
    access:              [read]

  - id:                  "0xF18C"
    name:                "ECUSerialNumber"
    data_length:         4
    min_session:         extended
    write_security_level: 1          # Level 1 unlock required to write
    access:              [read, write]

dtcs:
  - code:        "0xC00100"
    name:        "OvervoltageFault"
    description: "Pack voltage exceeded OV threshold"

routines:
  - id:            "0xFF00"
    name:          "ECU_SelfTest"
    description:   "Run internal self-test"
    min_session:   extended
    security_level: 0
    support:       ["start", "results"]
```

### Context variables passed to `uds_init.c.j2`

`build_uds_init_context()` in `codegen.py` builds the following variables:

| Variable | Type | Source |
|---|---|---|
| `ecu_name` | str | `metadata.ecu_name` |
| `version` | str | `metadata.version` |
| `p2_server_max_ms` | int | `timing.p2_server_max_ms` |
| `p2_star_server_max_ms` | int | `timing.p2_star_server_max_ms` |
| `s3_server_timeout_ms` | int | `timing.s3_server_timeout_ms` |
| `can_rx_id` | int | `can.rx_can_id` (default 0x7DF) |
| `can_tx_id` | int | `can.tx_can_id` (default 0x7E8) |
| `dids` | list | `_build_did_list(cfg)` |
| `dtcs` | list | `_build_dtc_list(cfg)` |
| `routines` | list | `_build_routine_list(cfg)` |
| `safeboot_enabled` | bool | `safeboot.enabled` (default `False`) |
| `safeboot_platform` | str | `safeboot.platform` (default `"zephyr"`) |
| `safeboot_max_block` | int | `safeboot.max_block_length` (default `256`) |

---

## 6. Template Catalogue

All 14 templates in `tools/templates/`:

| Template | Output file | Description |
|---|---|---|
| `did_handlers.c.j2` | `did_handlers.c` | DID callback stub implementations |
| `did_handlers.h.j2` | `did_handlers.h` | DID callback prototypes |
| `did_safety_wrappers.c.j2` | `did_safety_wrappers.c` | ASIL-B 5-step wrapper implementations |
| `did_safety_wrappers.h.j2` | `did_safety_wrappers.h` | Wrapper prototypes |
| `uds_init.c.j2` | `uds_init.c` | Full stack init sequence (Steps 1–7) |
| `uds_init.h.j2` | `uds_init.h` | `uds_generated_init()` prototype |
| `generated_config.h.j2` | `generated_config.h` | CAN IDs, timing, counts, ECU metadata |
| `safety_config.h.j2` | `safety_config.h` | ASIL-B `_Static_assert` guards, DID counts |
| `test_did_XXXX.py.j2` | `test_did_XXXX.py` | Per-DID pytest (one file per DID) |
| `test_routine_XXXX.py.j2` | `test_routine_XXXX.py` | Per-routine pytest (one file per routine) |
| `test_services.py.j2` | `test_services.py` | Session, security, DTC service tests |
| `test_firmware_services.py.j2` | `test_firmware_services.py` | Firmware harness pytest |
| `conftest.py.j2` | `conftest.py` | Simulator transport fixture |
| `conftest_firmware.py.j2` | `conftest_firmware.py` | Firmware harness transport fixture |

---

## 7. Generated C Files

### `generated/did_handlers.c/.h`

One read/write stub per DID. Each stub is marked with a `TODO [APPLICATION]` comment instructing the integrator to replace the stub body with real sensor reads, CAN signal decoding, or NVM accesses.

```c
/* GENERATED — do not edit manually. Regenerate: codegen.py --config ... */

uds_status_t did_vehicleidentificationnumber_read(uint8_t *buf, uint16_t len)
{
    /* TODO [APPLICATION]: Replace with real VIN read from NVM */
    (void)memset(buf, 0x20U, (size_t)len);
    return UDS_STATUS_OK;
}
```

### `generated/did_safety_wrappers.c/.h`

One wrapper function per DID per access direction. Implements the full 5-step validation chain. The wrapper is the only entry point through which a DID handler may be invoked — no service handler calls a DID callback directly.

### `generated/routine_handlers.c/.h`

One start/stop/results stub per routine. Same `TODO [APPLICATION]` pattern as DID handlers.

### `generated/uds_init.c`

The complete stack initialisation sequence, emitted as `uds_generated_init(can_transport_t *can, uint32_t rx_id, uint32_t tx_id)`. The sequence:

1. `uds_safety_init()` — ASIL-B check engine (REQ-SAFE-005)
2. `isotp_init()` — ISO-TP channel over CAN transport
3. `uds_server_init()` — UDS dispatcher
4. `dtc_database_init()` — DTC static table
5. `dtc_mirror_init()` + `dtc_mirror_load()` — NVM persistence (REQ-DTC-NVM-01)
6. `did_database_init()` — DID static table
7. `did_handlers_register_all()` — link handler stubs into DID table
8. Per-DTC `dtc_database_register()` loop
9. Per-routine `routine_database_register()` loop
10. **Step 5.7 — SafeBoot (conditional):** `zephyr_flash_ops_init()` is generated only when `safeboot.enabled: true` in the YAML. When enabled, it registers the MCUboot secondary-slot flash ops table so that services 0x34/0x36/0x37 accept firmware downloads. When disabled (default), a documentation comment is emitted explaining how to enable it — no code is generated and all three download services remain locked out with NRC 0x22.

The SafeBoot conditional in `uds_init.c.j2`:

```jinja
{% if safeboot_enabled %}
#include "zephyr_flash_ops.h"
{% endif %}

...

{% if safeboot_enabled %}
    status = zephyr_flash_ops_init();
    if (status != UDS_STATUS_OK) {
        return status;
    }
{% endif %}
```

### `generated/generated_config.h`

Compile-time constants derived from YAML:

```c
#define GEN_ECU_NAME               "BasicECU"
#define GEN_CAN_RX_ID              (2015U)    /* 0x7DF */
#define GEN_CAN_TX_ID              (2024U)    /* 0x7E8 */
#define GEN_P2_SERVER_MAX_MS       (25U)
#define GEN_S3_SERVER_TIMEOUT_MS   (5000U)
#define GEN_DID_COUNT              (5U)
#define GEN_DTC_COUNT              (2U)
```

### `generated/safety_config.h`

Compile-time ASIL-B assertions. Prevents accidental disabling of safety checks:

```c
#define GEN_SAFETY_DID_COUNT       (5U)
#define UDS_STACK_VERSION          "1.0.0"

_Static_assert(GEN_SAFETY_DID_COUNT == GEN_DID_COUNT,
               "Safety wrapper count must equal DID count");
_Static_assert(UDS_SAFETY_ENABLE_SESSION_CHECK == 1,
               "REQ-SAFE-002: session check must remain enabled");
```

---

## 8. Generated Test Files

When `--test-gen` is active, codegen produces a complete pytest suite in `generated/tests/`:

| File | Tests generated |
|---|---|
| `test_did_XXXX.py` | Read happy path, write happy path (if writable), session gate NRC, security gate NRC (if applicable), length error NRC |
| `test_routine_XXXX.py` | startRoutine happy path, session gate, security gate (if `security_level > 0`), stopRoutine not-supported (if absent from `support`) |
| `test_services.py` | Session switching (all transitions), TesterPresent, SecurityAccess full unlock flow, ClearDTC, ReadDTCByStatusMask, ControlDTCSetting |
| `test_firmware_services.py` | Same as `test_services.py` but runs against the compiled C harness binary |
| `conftest.py` | `IsoTpTransport` fixture with simulator, virtual, SocketCAN, and firmware backends |
| `conftest_firmware.py` | `FirmwareIsoTpTransport`: launches harness binary, waits for `READY` sentinel, connects over `AF_UNIX SOCK_SEQPACKET` |

Tests run in two modes controlled by the `--can-interface` pytest flag:

```bash
# Simulator mode (no hardware, fast)
pytest . --can-interface=simulator

# Firmware mode (real compiled C stack)
pytest test_firmware_services.py --can-interface=firmware
pytest test_routine_*.py --can-interface=firmware
```

---

## 9. Generated GUI Catalog

When `--gui-types` is active, codegen writes `gui/src/generated/catalog.ts`:

```typescript
// GENERATED — DO NOT EDIT MANUALLY
// Source: diagnostics_config.yaml (ECU: BasicECU, version: 1.1.0)
// Tool:   tools/codegen.py --gui-types

import type { DidInfo, RoutineInfo } from '../types';

export const DID_CATALOG: DidInfo[] = [
  { hex: '0xF190', name: "Vehicle Identification Number", length: 17, type: 'ascii' },
  { hex: '0x0C00', name: "Engine Speed", length: 2, type: 'numeric' },
  // ...
];

export const ROUTINE_CATALOG: RoutineInfo[] = [
  { id: '0xFF00', name: "ECU_SelfTest", description: "...",
    minSession: 'extended', securityLevel: 0, support: ['start', 'results'] },
  // ...
];
```

`RoutinesPanel.tsx` and `DidsPanel.tsx` import directly from this generated file, so the dashboard always reflects the current YAML configuration without manual synchronisation. The GUI build CI job regenerates this file before running `npm run typecheck`.

---

## 10. CLI Reference

```bash
python3 tools/codegen.py \
  --config  <yaml>         Path to diagnostics_config.yaml (required)
  --out     <dir>          Output directory for C files (default: generated/)
  --safety-wrappers        Generate ASIL-B did_safety_wrappers.c/.h
  --asil-level  B          ASIL level (default: B); enables stricter constraints
  --test-gen               Generate pytest suite in <out>/tests/
  --gui-types              Generate gui/src/generated/catalog.ts
  --gui-out     <dir>      Override GUI output directory
  --no-manifest            Skip manifest.json (use in CI)
  --dry-run                Validate config only; write no files
  --template-dir <dir>     Override tools/templates/ location
```

Example — BMS ECU with full test generation:

```bash
python3 tools/codegen.py \
  --config  examples/bms_ecu/diagnostics_config.yaml \
  --out     examples/bms_ecu/generated/ \
  --safety-wrappers \
  --asil-level B \
  --test-gen \
  --gui-types \
  --gui-out gui/src/generated/ \
  --no-manifest
```

---

## 11. Build System Integration

Generated files are included in the Zephyr CMake build via `target_sources()`:

```cmake
target_sources(app PRIVATE
    ${DIAG_GENERATED_DIR}/did_handlers.c
    ${DIAG_GENERATED_DIR}/did_safety_wrappers.c
    ${DIAG_GENERATED_DIR}/routine_handlers.c
    ${DIAG_GENERATED_DIR}/uds_init.c
)
```

The root `CMakeLists.txt` defines a `run_codegen` custom target that re-runs `codegen.py` when any `.j2` template or the YAML changes (via `CODEGEN_SENTINEL` dependency tracking). To skip codegen during iterative builds:

```bash
west build -b native_sim examples/basic_ecu -- -DDIAG_SKIP_CODEGEN=ON
```

---

## 12. Validation and Error Handling

The generator validates the YAML configuration before writing any file. The following conditions are fatal errors that abort generation:

| Check | Error example |
|---|---|
| Duplicate DID ID | `VALIDATION ERROR: dids[2]: duplicate id 0xF190` |
| Invalid DID ID format | `dids[0]: id must be a hex value in [0x0000, 0xFFFF]` |
| Invalid DTC code format | `dtcs[1]: code must be a 6-digit hex value` |
| ASIL-B: write without security | `dids[3]: write DIDs require write_security_level > 0 (ASIL-B)` |
| ASIL-B: data length exceeded | `dids[4]: data_length 128 exceeds ASIL-B maximum of 64` |
| Missing required field | `dids[0]: required field 'name' is missing` |
| Invalid session name | `dids[1]: min_session 'factory' is not a valid session` |
| Routine missing 'start' | `routines[0]: 'start' must always be in support list` |

Warnings (non-fatal) are printed for advisory issues such as missing CAN configuration (defaults applied) or no routines configured.

---

## 13. Regeneration Workflow

Generated files must not be manually edited. Any manual change will be overwritten the next time codegen runs.

```
1. Edit diagnostics_config.yaml
         │
         ▼
2. python3 tools/codegen.py --config ... --out generated/ \
          --safety-wrappers --asil-level B --test-gen --gui-types
         │
         ▼
3. Review generated diff (git diff generated/)
         │
         ▼
4. Update DID/routine callback stubs in generated/did_handlers.c
   (only the TODO [APPLICATION] bodies — the rest is generated)
         │
         ▼
5. Rebuild firmware: west build -b native_sim examples/basic_ecu
```

---

## 14. Generated Code Constraints

All generated C modules adhere to the following rules, enforced by the templates:

| Constraint | Rationale |
|---|---|
| No dynamic allocation | ASIL-B / MISRA C:2012 Rule 21.3 |
| No recursion | Deterministic stack depth |
| Static tables only | Predictable memory layout |
| All functions return `uds_status_t` | Explicit error propagation |
| No blocking operations | Real-time compatibility |
| `GENERATED — DO NOT EDIT` header in every file | Prevents manual drift |
| Traceability tags (`REQ-SAFE-*`, `REQ-DL-*`) in comments | ISO 26262 traceability |
