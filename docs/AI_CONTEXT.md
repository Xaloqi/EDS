# AI_CONTEXT.md — Xaloqi EDS

> Add this file to your AI project (Claude, Cursor workspace, Copilot context, etc.) to enable
> AI-assisted EDS development. The AI assistant will use this context to generate correct YAML,
> C code, and CLI commands without reading all source files first.

---

## What is EDS?

Xaloqi EDS (Xaloqi Embedded Diagnostic Suite) is a production-grade ISO 14229 (UDS) +
ISO 15765-2 (ISO-TP) diagnostics stack for embedded RTOS targets. It is YAML-driven: you
describe your DIDs, DTCs, and routines in YAML, run the code generator, and receive
ASIL-B safety-wrapped C code ready to compile into your ECU firmware.

**Version:** v1.7.0
**Target RTOS:** Zephyr v3.7+ · FreeRTOS (any version with static allocation support)
**Target boards:** native_sim (CI/dev) · STM32 Nucleo-H743ZI2 (hardware) · QEMU ARM Cortex-M4 (FreeRTOS CI)
**Transport:** ISO-TP over CAN (default) · DoIP over Ethernet/TCP (v1.6.0+)
**OpenSOVD CDA:** `--sovd` flag generates `sovd_cda.json` (v1.7.0+)

---

## Repository Structure

```
EDS/
├── core/                   # UDS server, session manager, security manager, service dispatcher
├── transport/              # ISO-TP state machine + CAN transport binding
│   └── doip/               # DoIP server (ISO 13400-2) — Zephyr + FreeRTOS+LwIP bindings
├── config/                 # DID database, DTC database, NVM DTC mirror
├── platform/
│   ├── platform_api.h      # Common interface implemented by both HALs
│   ├── zephyr/             # Zephyr RTOS port (threads, timers, CAN, NVM, WDT)
│   └── freertos/           # FreeRTOS port (customer provides CAN send + NVM ops)
├── generated/              # Output of codegen.py — DO NOT hand-edit
├── tools/
│   ├── codegen.py          # Main code generator (YAML → C/H)
│   ├── testgen.py          # Test suite generator (YAML → pytest + CANoe CAPL)
│   ├── config_parser.py    # YAML validation schema
│   └── templates/          # Jinja2 templates for all generated files
├── examples/
│   ├── basic_ecu/                    # Minimal reference ECU — start here (Zephyr)
│   ├── basic_ecu_freertos/           # Same YAML, FreeRTOS platform — start here (FreeRTOS)
│   ├── sensor_ecu/                   # Sensor → DID → DTC pattern (Zephyr sensor API)
│   ├── sensor_ecu_freertos/          # Same YAML as sensor_ecu, FreeRTOS platform
│   ├── safeboot_ecu/                 # MCUboot DFU over UDS (safeboot.enabled: true)
│   ├── robot_joint_controller_ecu/   # Robotics positioning example
│   ├── bms_ecu/                      # Battery Management System (24 DIDs)
│   ├── motor_controller_ecu/         # Motor controller (27 DIDs)
│   └── ardep_ecu/                    # Mercedes ARDEP reference platform (35 DIDs)
├── ide/
│   └── vscode-extension/   # VS Code extension (YAML validation, hover docs, Run Codegen)
├── gui/                    # React/TypeScript live dashboard + WebSocket ECU bridge
├── tests/
│   ├── unit_runnable/      # 35 Unity C unit test modules
│   ├── integration/        # Python ISO-TP/UDS flow tests
│   └── harness/            # 68 build harness tests
├── scripts/
│   ├── build_tests.sh
│   └── build_harness.sh
└── .github/workflows/ci.yml   # 7-job public CI pipeline (unit, integration, static, zephyr×2, freertos, harness)
```

**Golden rule:** Never hand-edit files in `generated/`. They are overwritten on every codegen run.

---

## Complete YAML Configuration Schema

All fields are optional unless marked **[required]**.

```yaml
# ─── ECU Metadata ────────────────────────────────────────────────────────────
metadata:
  ecu_name:    "MyECU"          # [required] ECU display name; used in generated comments
  version:     "1.0.0"          # [required] Firmware version string
  description: "My ECU"         # Human-readable description (multi-line OK with >)

# ─── Timing ──────────────────────────────────────────────────────────────────
timing:
  p2_server_max_ms:       25    # ISO 14229 P2 server max response time (default 25)
  p2_star_server_max_ms:  5000  # P2* extended response time (default 5000)
  s3_server_timeout_ms:   5000  # S3 session keep-alive timeout (default 5000)

# ─── SafeBoot — MCUboot DFU over UDS ─────────────────────────────────────────
# Omit this block entirely if your ECU does not do firmware updates over UDS.
safeboot:
  enabled: true               # Set true → codegen generates zephyr_flash_ops_init()
                              # in uds_init.c, wiring 0x34/0x36/0x37 to MCUboot flash
  platform: zephyr            # "zephyr" | "freertos"
  max_block_length: 256       # Bytes per TransferData block (256 recommended for CAN classic)

# ─── DIDs — Data Identifiers ─────────────────────────────────────────────────
dids:
  - id:   "0xF190"            # [required] 2-byte hex DID identifier
    name: "VehicleIdentificationNumber"  # [required] Used as C symbol prefix
    description: "17-byte VIN"           # Optional — shown in hover docs
    access:                   # [required] One or more of: read, write
      - read
    min_session: "default"    # Minimum session: "default" | "extended" | "programming"
    read_security_level:  0   # 0 = no auth required; 1+ = SecurityAccess level needed
    write_security_level: 1
    data_length: 17           # [required] Data length in bytes

  - id:   "0x0300"
    name: "CalibrationData"
    access:
      - read
      - write
    min_session: "extended"
    read_security_level:  0
    write_security_level: 2   # Requires SecurityAccess level 2 to write
    data_length: 8

# ─── DTCs — Diagnostic Trouble Codes ─────────────────────────────────────────
dtcs:
  - code:        "0x123456"   # [required] 3-byte hex DTC code
    description: "Sensor reading out of range"
    severity:    "check_at_next_halt"  # "immediate_display" | "check_at_next_halt" | "check_at_next_halt_ecu_reset"

# ─── Routines ────────────────────────────────────────────────────────────────
routines:
  - id:   "0xFF00"            # [required] 2-byte hex routine identifier
    name: "ClearCalibration"  # [required]
    description: "Reset calibration to factory defaults"
    min_session:    "extended"
    security_level: 1
    support:        ["start", "results"]  # "start" | "stop" | "results"
```

---

## Code Generator CLI Reference

```bash
# Standard generation (safety wrappers on by default):
python3 tools/codegen.py \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/

# With explicit ASIL-B safety wrappers:
python3 tools/codegen.py \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/ \
    --safety-wrappers \
    --asil-level B

# Also generate pytest test suite:
python3 tools/codegen.py \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/ \
    --safety-wrappers \
    --test-gen

# Dry run — validate YAML only, write nothing:
python3 tools/codegen.py \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/ \
    --dry-run

# Generate GUI TypeScript catalog alongside C files:
python3 tools/codegen.py \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/ \
    --gui-types

Options:
  --config, -c PATH       Path to diagnostics_config.yaml  [required]
  --out,    -o DIR        Output directory (default: generated/)
  --safety-wrappers       Generate ASIL-B 5-step safety wrapper files
  --asil-level [A|B]      ASIL target level (default: B)
  --test-gen              Also generate pytest test suite into <out>/tests/
  --gui-types             Also generate gui/src/generated/catalog.ts
  --sovd                  Generate sovd_cda.json (OpenSOVD 1.0 CDA) alongside C output
  --no-manifest           Skip JSON manifest file
  --dry-run               Validate only — write nothing
```

### Generated Files

| File | Contents |
|---|---|
| `generated_config.h` | Compile-time constants (CAN IDs, DID count, timing) |
| `did_handlers.h` | DID handler function prototypes |
| `did_handlers.c` | Static DID registration table + handler stubs |
| `did_safety_wrappers.h` | ASIL-B 5-step wrapper prototypes |
| `did_safety_wrappers.c` | 5-step wrapper implementations (with `--safety-wrappers`) |
| `routine_handlers.h` | Routine handler prototypes |
| `routine_handlers.c` | Static routine registration table |
| `safety_config.h` | ASIL compile-time macro configuration |
| `uds_init.h` | `uds_generated_init()` declaration |
| `uds_init.c` | Full UDS + DTC + DID + flash-ops initialisation sequence |
| `sovd_cda.json` | OpenSOVD 1.0 CDA (DIDs, DTCs, routines, transport info) — only with `--sovd` |

---

## Test Generator CLI Reference

```bash
# pytest suite only (default):
python3 tools/testgen.py \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/

# pytest + CANoe CAPL:
python3 tools/testgen.py --capl \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/

# CANoe CAPL only:
python3 tools/testgen.py --capl --capl-only \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/

Options:
  --capl          Generate CANoe CAPL .can test scripts
  --capl-only     Suppress pytest output (CAPL only)
  --verbose       Print generated file list
```

### Generated pytest test coverage (per DID)

- `Read_HappyPath` — positive response, correct SID/DID echo
- `Read_ResponseLen` — response length == data_length + 3
- `Read_WrongSession` — NRC 0x7F (only if `min_session > default`)
- `Read_SecurityLocked` — NRC 0x33 (only if `read_security_level > 0`)
- `Read_MultipleTimes` — 5 consecutive reads, consistent length
- `Write_HappyPath` — positive write + readback (only if write access)
- `Write_WrongLength` — NRC 0x13 (only if write access)
- `Write_WrongSession` — NRC 0x7F (only if write access + session constraint)
- `Write_SecurityLocked` — NRC 0x33 (only if `write_security_level > 0`)

---

## Integration Pattern — Zephyr

```c
// In your ECU main.c or diagnostics task:
#include "uds_init.h"

void diagnostics_task(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    uds_status_t status = uds_generated_init(
        zephyr_can_get_transport(),
        GEN_CAN_RX_ID,   /* from generated_config.h */
        GEN_CAN_TX_ID);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("UDS init failed: %d", status);
        return;
    }

    while (1) {
        uds_server_process();
        k_sleep(K_MSEC(1));
    }
}

K_THREAD_DEFINE(diag_thread, 2048,
    diagnostics_task, NULL, NULL, NULL,
    K_PRIO_COOP(7), 0, 0);
```

---

## Integration Pattern — FreeRTOS

```c
// In your ECU main.c:
#include "platform_api.h"
#include "freertos_can.h"
#include "uds_init.h"
#include "generated_config.h"

// 1. Provide your CAN send function:
static uds_status_t my_can_send(const eds_can_frame_t *frame)
{
    // e.g. HAL_CAN_AddTxMessage(&hcan1, ...) for STM32
    return UDS_STATUS_OK;
}

// 2. Optionally provide NVM ops (omit to use RAM stub — data lost on reset):
static const eds_nvm_ops_t my_nvm = {
    .read     = my_flash_read,
    .write    = my_flash_write,
    .is_ready = my_flash_is_ready,
};

int main(void)
{
    // 3. Init EDS platform:
    eds_platform_init(&(eds_platform_cfg_t){
        .can_send            = my_can_send,
        .nvm                 = my_nvm,          // omit to use RAM stub
        .uds_task_stack_size = 2048U,
        .uds_task_priority   = 5U,
    });

    // 4. Init UDS stack (same call as Zephyr):
    uds_generated_init(
        freertos_can_get_transport(),
        GEN_CAN_RX_ID,
        GEN_CAN_TX_ID);

    // 5. Start FreeRTOS scheduler:
    vTaskStartScheduler();
}

// 6. In your CAN RX interrupt or callback — feed frames to EDS:
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    eds_can_frame_t frame;
    // ... fill frame from HAL ...
    eds_platform_can_input(&frame);   // ISR-safe, posts to RX queue
}
```

**Key differences from Zephyr:**
- Call `eds_platform_init()` before `uds_generated_init()` — mandatory
- Provide your CAN send function via `eds_platform_cfg_t.can_send`
- Feed incoming CAN frames via `eds_platform_can_input()` from your CAN ISR
- NVM ops are optional — RAM stub is used if omitted (development only; data lost on reset)
- `uds_generated_init()` and `uds_server_process()` calls are identical to Zephyr

---

## Integration Pattern — DoIP (ISO 13400-2)

DoIP transport is an additive v1.6.0 feature. The UDS core, safety wrappers, and YAML schema
are identical to CAN builds.

### YAML change

```yaml
ecu:
  transport: doip           # replaces default "can"
  doip:
    logical_address: "0xE400"
    source_address:  "0x0E00"
    port:            13400
```

### Generated code impact

With `EDS_DOIP_ONLY_BUILD=1` defined, `uds_init.c` skips `isotp_init()`.
Call `uds_generated_init(NULL, 0U, 0U)` — pass NULL for the CAN transport.

### Zephyr main.c pattern

```c
#include "platform_doip.h"
#include "doip_server.h"          // DOIP_PORT = 13400

int main(void)
{
    uds_generated_init(NULL, 0U, 0U);
    uds_server_ctx_t *srv = uds_generated_get_server();

    eds_doip_platform_start(0xE400U, DOIP_PORT, srv);
    // K_THREAD_DEFINE thread starts automatically
    return 0;
}
```

### FreeRTOS main.c pattern

```c
#include "platform_doip.h"   // platform/freertos/platform_doip.h
#include "doip_server.h"

int main(void)
{
    eds_platform_init(&(eds_platform_cfg_t){ .can_send = NULL, ... });
    uds_generated_init(NULL, 0U, 0U);
    uds_server_ctx_t *srv = uds_generated_get_server();

    eds_doip_platform_start_freertos(0xE400U, DOIP_PORT, srv, 4096U, 6U);
    vTaskStartScheduler();
    for (;;) {}
}
```

### Kconfig (Zephyr)

```
CONFIG_NETWORKING=y
CONFIG_NET_IPV4=y
CONFIG_NET_TCP=y
CONFIG_NET_SOCKETS=y
CONFIG_POSIX_API=y
CONFIG_NET_LOOPBACK=y   # native_sim only
CONFIG_NET_NATIVE=y     # native_sim only
```

### CMake additions (both RTOS)

```cmake
target_compile_definitions(app PRIVATE EDS_DOIP_ONLY_BUILD=1)
target_sources(app PRIVATE
    ${DIAG_ROOT}/transport/doip/doip_server.c
    ${DIAG_ROOT}/transport/doip/zephyr_lwip.c        # Zephyr
    # ${DIAG_ROOT}/transport/doip/freertos_lwip.c    # FreeRTOS
    ${DIAG_ROOT}/platform/zephyr/platform_doip.c     # Zephyr
    # ${DIAG_ROOT}/platform/freertos/platform_doip.c # FreeRTOS
)
```

### Testing with xaloqi-tester DoipBus

```python
from xaloqi.tester import UdsTester, DoipBus, Session
async with UdsTester(DoipBus("127.0.0.1"), rx_id=0xE400, tx_id=0x0E00) as ecu:
    await ecu.change_session(Session.EXTENDED)
    vin = await ecu.read_did(0xF190)
```

### Example ECUs

| Example | RTOS | Transport |
|---|---|---|
| `examples/basic_ecu_doip/` | Zephyr | DoIP (native_sim loopback) |
| `examples/basic_ecu_doip_freertos/` | FreeRTOS + LwIP | DoIP |

### Checklist for a new DoIP ECU (Zephyr)

1. Add `ecu.transport: doip` to `diagnostics_config.yaml`
2. Add networking Kconfig (`CONFIG_NETWORKING=y` etc.)
3. Add `EDS_DOIP_ONLY_BUILD=1` to `target_compile_definitions`
4. Add DoIP sources to `target_sources` (doip_server.c, zephyr_lwip.c, platform_doip.c)
5. Include `transport/doip` and `platform/zephyr` in include paths
6. Call `eds_doip_platform_start(logical_addr, DOIP_PORT, srv)` in `main()`
7. Copy `examples/basic_ecu_doip/generated/` as your starting generated files
8. Regenerate with codegen.py when your YAML stabilises


---

## DID Handler Implementation

```c
// In your application code (e.g. src/did_handlers_impl.c):
#include "did_handlers.h"
#include <string.h>

// Read handler — called after all 5 ASIL-B safety checks pass
uds_status_t vehicle_identification_number_read(
    uint8_t *data, uint16_t *length, uint16_t max_length)
{
    static const char vin[] = "1HGBH41JXMN109186";
    uint16_t vin_len = (uint16_t)(sizeof(vin) - 1U);
    if (max_length < vin_len) { return UDS_STATUS_ERR_LENGTH; }
    memcpy(data, vin, vin_len);
    *length = vin_len;
    return UDS_STATUS_OK;
}

// Write handler — called after all 5 safety checks including security level
uds_status_t calibration_data_write(const uint8_t *data, uint16_t length)
{
    if (length != 8U) { return UDS_STATUS_ERR_LENGTH; }
    nvm_write(NVM_ADDR_CALIBRATION, data, length);
    return UDS_STATUS_OK;
}
```

---

## DTC Reporting from Application Code

```c
#include "config/dtc_database.h"

// Set a DTC active (e.g. from a sensor monitoring task):
dtc_database_set_status(DTC_SENSOR_FAULT, DTC_STATUS_TEST_FAILED);

// Clear when fault resolves:
dtc_database_set_status(DTC_SENSOR_FAULT, DTC_STATUS_CLEAR);
```

---

## Sensor → DID → DTC Pattern

`examples/sensor_ecu/` demonstrates the complete pattern for zone controllers that
read real sensors and expose them as UDS DIDs with automatic DTC activation.

```c
// src/sensor_monitor.c — runs as a Zephyr thread every 100ms
static void sensor_monitor_thread(void *p1, void *p2, void *p3)
{
    struct sensor_value val;
    while (1) {
        sensor_sample_fetch(temp_dev);
        sensor_channel_get(temp_dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
        int32_t temp_c = val.val1;

        if (temp_c > TEMP_THRESHOLD_HIGH) {
            dtc_database_set_status(DTC_D00101, DTC_STATUS_TEST_FAILED);
        } else {
            dtc_database_set_status(DTC_D00101, DTC_STATUS_CLEAR);
        }
        k_sleep(K_MSEC(100));
    }
}
```

The DID read handler for DID 0xD001 (`AmbientTemperature`) returns the current sensor
value. When the tester sends `0x19 02 <status_mask>`, any active DTCs are returned.
On `native_sim`, the stub implementation cycles through normal → fault → recovery to
demonstrate DTC behaviour without hardware.

---

## SafeBoot — MCUboot DFU over UDS

Setting `safeboot.enabled: true` causes codegen to generate `zephyr_flash_ops_init()`
automatically in `uds_init.c`, wiring the MCUboot secondary-slot flash driver into the
UDS transfer services. No manual flash ops registration required.

**DFU sequence (7 steps):**
```
1. DiagnosticSessionControl  0x10 0x02       → enter programming session
2. SecurityAccess            0x27 0x01       → request seed
3. SecurityAccess            0x27 0x02 <key> → unlock level 1
4. RequestDownload           0x34 ...        → erase secondary slot; receive block size
5. TransferData              0x36 × N        → write firmware blocks
6. RequestTransferExit       0x37            → verify CRC-32, accept image
7. ECUReset                  0x11 0x01       → reboot → MCUboot swaps slots
```

**YAML to enable:**
```yaml
safeboot:
  enabled: true
  platform: zephyr   # or "freertos"
  max_block_length: 256
```

Safety enforcement is automatic: programming session + security level 1 required (ACL),
CRC-32 verified before image acceptance (REQ-FLASH-003), address validated against
MCUboot secondary slot bounds (REQ-FLASH-002). Primary slot is never written directly.

See `examples/safeboot_ecu/` for the full reference including `dfu_flash.py`.

---

## Build Commands Reference

```bash
# ── Code Generation ───────────────────────────────────────────────────────────
python3 tools/codegen.py \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/ \
    --safety-wrappers --asil-level B

# ── Test Generation ───────────────────────────────────────────────────────────
python3 tools/testgen.py \
    --config examples/basic_ecu/diagnostics_config.yaml \
    --out    examples/basic_ecu/generated/
cd examples/basic_ecu/generated/tests && pytest . -v

# ── Zephyr: native_sim (development / CI) ─────────────────────────────────────
west build -b native_sim examples/basic_ecu \
    -- -DDTC_OVERLAY_FILE=boards/native_sim.overlay
west build -t run

# ── Zephyr: STM32 Nucleo-H743ZI2 (hardware) ──────────────────────────────────
west build -b nucleo_h743zi2 examples/safeboot_ecu
west flash

# ── FreeRTOS build ────────────────────────────────────────────────────────────
cmake -B build_freertos \
    -DEDS_PLATFORM=freertos \
    -DFREERTOS_DIR=/path/to/FreeRTOS-Kernel \
    -GNinja \
    examples/basic_ecu_freertos
ninja -C build_freertos

# ── Unit Tests ────────────────────────────────────────────────────────────────
bash scripts/build_tests.sh          # 35 Unity modules

# ── Harness Tests ─────────────────────────────────────────────────────────────
bash scripts/build_harness.sh        # 68 harness tests

# ── GUI ───────────────────────────────────────────────────────────────────────
cd gui && npm ci && npm start        # Dev mode (WebSocket bridge + demo mode)
cd gui && npm run build              # Production build

# ── VS Code Extension ─────────────────────────────────────────────────────────
cd ide/vscode-extension && npm install && npx vsce package
code --install-extension eds-diagnostics-*.vsix
```

---

## Common Patterns

### Pattern 1: Add a read-only DID

```yaml
dids:
  - id:   "0xF1A0"
    name: "Odometer"
    access: [read]
    min_session: "default"
    read_security_level: 0
    data_length: 4
```

```c
uds_status_t odometer_read(uint8_t *data, uint16_t *length, uint16_t max_length)
{
    uint32_t km = app_get_odometer_km();
    data[0] = (uint8_t)(km >> 24); data[1] = (uint8_t)(km >> 16);
    data[2] = (uint8_t)(km >> 8);  data[3] = (uint8_t)(km);
    *length = 4U;
    return UDS_STATUS_OK;
}
```

---

### Pattern 2: Add a secured write DID

```yaml
dids:
  - id:   "0x0300"
    name: "CalibrationData"
    access: [read, write]
    min_session: "extended"
    read_security_level:  0
    write_security_level: 2
    data_length: 8
```

The generated 5-step wrapper enforces: DID exists → session is extended →
security level 2 is active → access is write → request length == 8.
Your handler is only called when all five checks pass.

---

### Pattern 3: Sensor fault → DTC

```yaml
dids:
  - id:   "0xD001"
    name: "AmbientTemperature"
    access: [read]
    min_session: "default"
    read_security_level: 0
    data_length: 1

dtcs:
  - code:        "0xD00101"
    description: "Ambient temperature — over-range"
    severity:    "check_at_next_halt"
```

```c
// In sensor monitoring thread:
if (temp_c > threshold_high) {
    dtc_database_set_status(DTC_D00101, DTC_STATUS_TEST_FAILED);
} else {
    dtc_database_set_status(DTC_D00101, DTC_STATUS_CLEAR);
}
```

---

### Pattern 4: Enable MCUboot DFU

```yaml
safeboot:
  enabled: true
  platform: zephyr
  max_block_length: 256
```

After codegen, `uds_init.c` automatically includes and calls `zephyr_flash_ops_init()`.
Services 0x34/0x36/0x37 become functional in the programming session with security level 1.

---

### Pattern 5: Security levels (supplier vs. OEM)

```yaml
dids:
  - id:   "0xF110"
    name: "ECUManufacturingDate"
    access: [read]
    min_session: "extended"
    read_security_level: 1   # Level 1 to read

  - id:   "0xF111"
    name: "ECUSerialNumber"
    access: [read, write]
    min_session: "extended"
    read_security_level:  0
    write_security_level: 2  # Level 2 to write
```

UDS tester to unlock level 2: send `27 03` → receive seed → compute AES-128-CMAC → send `27 04 <key>`.

---

## NRC Reference

| NRC | Hex | When EDS returns it |
|---|---|---|
| `generalReject` | 0x10 | Unrecoverable internal error |
| `serviceNotSupported` | 0x11 | SID not in dispatch table |
| `subFunctionNotSupported` | 0x12 | Unknown sub-function byte |
| `incorrectMessageLength` | 0x13 | Step 5: wrong DID data length |
| `conditionsNotCorrect` | 0x22 | Step 4: access permission check failed |
| `requestSequenceError` | 0x24 | SecurityAccess key without prior seed |
| `requestOutOfRange` | 0x31 | Step 1: DID not in database |
| `securityAccessDenied` | 0x33 | Step 3: required security level not active |
| `invalidKey` | 0x35 | Wrong key in SecurityAccess response |
| `exceededNumberOfAttempts` | 0x36 | Too many failed SecurityAccess attempts |
| `requiredTimeDelayNotExpired` | 0x37 | SecurityAccess delay timer active |
| `uploadDownloadNotAccepted` | 0x70 | SafeBoot: precondition not met |
| `generalProgrammingFailure` | 0x72 | Flash write or erase failure |
| `wrongBlockSequenceCounter` | 0x73 | TransferData block counter out of sequence |
| `serviceNotSupportedInActiveSession` | 0x7F | Step 2: session not in DID's allowed sessions |

---

## UDS Service Reference

| SID | Hex | Status | Notes |
|---|---|---|---|
| DiagnosticSessionControl | 0x10 | ✅ | 0x01 default, 0x02 programming, 0x03 extended |
| ECUReset | 0x11 | ✅ | Hard (0x01), soft (0x03) |
| ClearDiagnosticInformation | 0x14 | ✅ | Clears DTC mirror + RAM |
| ReadDTCInformation | 0x19 | ✅ | Sub-functions 0x01, 0x02, 0x06, 0x0A |
| ReadDataByIdentifier | 0x22 | ✅ | Multi-DID read supported |
| SecurityAccess | 0x27 | ✅ | AES-128-CMAC; odd=seed byte, even=key byte |
| CommunicationControl | 0x28 | ✅ | Enable/disable Rx/Tx |
| WriteDataByIdentifier | 0x2E | ✅ | Full ASIL-B wrapper chain |
| RoutineControl | 0x31 | ✅ | Start / Stop / RequestResult |
| RequestDownload | 0x34 | ✅ | Programming session + security level 1 required |
| TransferData | 0x36 | ✅ | Block transfer; CRC-32 verified at exit |
| RequestTransferExit | 0x37 | ✅ | Verifies CRC, accepts MCUboot image |
| TesterPresent | 0x3E | ✅ | Keep-alive; responseRequired sub-fn only |
| ControlDTCSetting | 0x85 | ✅ | DTCSettingOn / DTCSettingOff |

**Suppress-response bit (ISO 14229-1 §7.5.3):** When bit 7 of the sub-function byte is set (`0x80`),
the ECU must return no response. EDS enforces this for all 6 services that have a sub-function byte:
0x10 (DSC), 0x11 (ECUReset), 0x28 (CommunicationControl), 0x31 (RoutineControl),
0x3E (TesterPresent), 0x85 (ControlDTCSetting). The generated inline simulator and the C stack
both comply. (v1.7.0 fixed compliance for 0x11, 0x28, 0x31, 0x85 in the simulator.)

---

## Checklist for a New ECU (Zephyr — CAN transport)

1. Copy `examples/basic_ecu/` to `examples/my_ecu/`
2. Edit `examples/my_ecu/diagnostics_config.yaml` — add your DIDs, DTCs, routines
3. `python3 tools/codegen.py --config examples/my_ecu/diagnostics_config.yaml --out examples/my_ecu/generated/ --safety-wrappers`
4. Implement handler functions declared in `examples/my_ecu/generated/did_handlers.h`
5. Call `uds_generated_init()` at startup; `uds_server_process()` in your task loop
6. `python3 tools/testgen.py --config examples/my_ecu/diagnostics_config.yaml --out examples/my_ecu/generated/`
7. `cd examples/my_ecu/generated/tests && pytest . -v`
8. `bash scripts/build_tests.sh`
9. `west build -b native_sim examples/my_ecu -- -DDTC_OVERLAY_FILE=boards/native_sim.overlay`

## Checklist for a New ECU (FreeRTOS — CAN transport)

1. Copy `examples/basic_ecu_freertos/` to `examples/my_ecu_freertos/`
2. Edit `diagnostics_config.yaml` — **identical format to Zephyr**
3. Run codegen — **identical command to Zephyr** (codegen is platform-independent)
4. In `src/main.c`: implement `my_can_send()`, call `eds_platform_init()`, then `uds_generated_init()`
5. In your CAN RX ISR: call `eds_platform_can_input(&frame)`
6. Run testgen and pytest — **identical commands to Zephyr**
7. `cmake -B build -DEDS_PLATFORM=freertos -DFREERTOS_DIR=/path/to/FreeRTOS-Kernel -GNinja examples/my_ecu_freertos && ninja -C build`

---

## Robustness Test Campaign (v1.7.0)

439 pytest tests in `examples/basic_ecu/generated/tests/`, runnable without hardware:

```bash
cd examples/basic_ecu/generated/tests
pytest test_robustness_A_codegen.py \
       test_robustness_B_protocol.py \
       test_robustness_C_security.py \
       test_robustness_D_customer_journey.py \
       test_robustness_E_data_integrity.py \
       test_robustness_F_codegen_limits.py \
       test_robustness_G_resilience.py \
       test_robustness_H_protocol_precision.py \
       test_robustness_I_nrc_wdbi_sa.py \
       test_robustness_J_sovd_cda.py \
       test_robustness_K_error_quality.py \
       test_robustness_L_codegen_output_fidelity.py \
       --can-interface=simulator -q
```

| Phase | Tests | What it covers |
|---|---|---|
| A | 22 | Generated file presence, C safety markers, GCC syntax |
| B | 42 | Session transitions, TesterPresent, ECUReset, all 14 service NRCs |
| C | 21 | CMAC SecurityAccess unlock/lockout/replay |
| D | 30 | Customer workflow (fresh YAML → codegen → pytest), all 11 ECU examples |
| E | 35 | DID read/write integrity, DTC lifecycle, session isolation |
| F | 54 | Max DID/DTC/routine counts, GCC syntax gate for all 11 ECU C files |
| G | 47 | Malformed PDU resilience, CMAC round-trip, suppress-response bit, YAML↔simulator consistency |
| H | 41 | DSC timing precision, multi-DID RDBI batch, DTC record format, routine lifecycle |
| I | 34 | NRC 3-byte format/SID echo, WDBI check ordering, SA level isolation |
| J | 43 | SOVD CDA semantic fidelity: structure, DID/DTC/routine fields, DoIP fields, idempotency |
| K | 35 | Codegen error quality: bad YAML exits non-zero with actionable keyword in stderr |
| L | 35 | Codegen output fidelity: generated C contains correct DID IDs, data lengths, timing constants, DTC severity bytes, routine flags |

---

## Common Mistakes

### "DID not found" at runtime (NRC 0x31)
Always re-run codegen after any YAML change.

### "Security access denied" (NRC 0x33) unexpectedly
DID has `read_security_level: N > 0`. Tester must complete SecurityAccess first:
send `27 (2N-1)` → receive seed → compute AES-128-CMAC → send `27 (2N)`.

### FreeRTOS: UDS stack unresponsive
Wrong init order. Required sequence: `eds_platform_init()` → `uds_generated_init()` → `vTaskStartScheduler()`.

### FreeRTOS: CAN frames not received
`eds_platform_can_input()` not called from CAN RX interrupt. The FreeRTOS HAL does not
auto-register a CAN RX callback — customer must call it explicitly.

### DoIP: ECU not reachable after init

Most common cause: networking not up before DoIP task unblocks. The FreeRTOS binding
has a 500 ms startup delay; Zephyr binding starts after `K_THREAD_DEFINE` delay.
Verify `CONFIG_NET_LOOPBACK=y` and `CONFIG_POSIX_API=y` for native_sim.

### DoIP: `isotp_init` linker error

You forgot `EDS_DOIP_ONLY_BUILD=1` in `target_compile_definitions`. This macro gates
the `isotp_init()` call in generated `uds_init.c`.

### DoIP: routing activation rejected

Default tester address is `0x0E00` (xaloqi-tester DoipBus default). Your
`uds_generated_init(NULL, 0, 0)` must have completed before the first connection.

### SafeBoot: 0x34 returns NRC 0x22
`safeboot.enabled: true` not in YAML, or codegen not re-run. Verify
`zephyr_flash_ops_init()` is present in `generated/uds_init.c`.

### Build fails: "CONFIG_CAN_LOOPBACK not set" (Zephyr)
Add to board `.conf`: `CONFIG_CAN_LOOPBACK=y`, `CONFIG_CAN=y`, `CONFIG_ISOTP=y`.

---

## GUI

The `gui/` directory contains a React/TypeScript live dashboard with eight panels:
**Overview** (stack status, session, security level) · **DIDs** (read/write with live
sparklines) · **DTCs** (active faults, status byte) · **Security** (seed/key exchange
walkthrough) · **Routines** (start/stop/results) · **Raw Frames** (ISO-TP frame log) ·
**Console** (UDS command terminal) · **Configurator** (YAML editor + Run Codegen button)

The WebSocket bridge (`gui/server/bridge.py`) connects the React app to a running ECU.
Demo mode runs without hardware: `bash gui/start-demo.sh`.

---

## VS Code Extension

```bash
cd ide/vscode-extension && npm install && npx vsce package
code --install-extension eds-diagnostics-*.vsix
```

Features: inline validation squiggles · hover docs on every YAML key ·
`EDS: Run Codegen` (`Ctrl+Shift+P`) · status bar error/warning count.

---

## MCP Server — AI Coding Assistant Integration

`tools/mcp_server.py` exposes Xaloqi EDS tools to Claude, Cursor, GitHub Copilot,
and any MCP-compatible host. Delivered with Developer and Professional licenses.
License is checked at startup via `_license.py`.

### Tools

| Tool | What it does |
|---|---|
| `generate_did_config` | Generate a complete `diagnostics_config.yaml` scaffold from ECU parameters. Use this first when helping a customer configure a new ECU. |
| `run_codegen` | Run `codegen.py` on a config file. License enforced inside codegen — no duplication. Returns success flag, generated file list, stdout/stderr. |
| `validate_asil_b` | Validate YAML against ASIL-B constraints via `codegen.py --dry-run`. Returns structured `{valid, violations, warnings}`. |
| `explain_uds_error` | Explain a UDS NRC code with EDS-specific trigger and fix guidance. Accepts `0x31`, `31`, or decimal. |

### Start the server

```bash
# License key required (XALOQI_LICENSE_SKIP=1 for CI/testing only)
python3 tools/mcp_server.py
```

### Claude Desktop integration

Add to `~/Library/Application Support/Claude/claude_desktop_config.json`
(macOS) or `%APPDATA%\Claude\claude_desktop_config.json` (Windows):

```json
{
  "mcpServers": {
    "xaloqi-eds": {
      "command": "python3",
      "args": ["/absolute/path/to/EDS/tools/mcp_server.py"]
    }
  }
}
```

Restart Claude Desktop. The four tools appear automatically.

### Cursor integration

Add to `.cursor/mcp.json` in your repo root:

```json
{
  "mcpServers": {
    "xaloqi-eds": {
      "command": "python3",
      "args": ["tools/mcp_server.py"]
    }
  }
}
```

### Example Claude prompt

```
Using the xaloqi-eds MCP server:
1. Generate a diagnostics_config.yaml for a BMS ECU with DIDs for cell voltage
   (0x2001, 2 bytes), pack temperature (0x2002, 1 byte), and state of charge
   (0x2003, 1 byte). Add a DTC for cell undervoltage (0xC10100).
2. Validate it against ASIL-B constraints.
3. Run codegen with safety wrappers.
```

### Protocol

JSON-RPC 2.0 over stdio (newline-delimited). No external MCP SDK required.
All four tools available on Developer and Professional tiers — no tier gating.

### CI

Job 18 (`mcp-server-tests`) in `.github/workflows/ci.yml` runs
`tools/ci_mcp_test.py` with `XALOQI_LICENSE_SKIP=1`. 50 protocol and
tool-level assertions. Requires `pyyaml` only.

---

*EDS v1.7.0 — Developer €690/yr · Professional €1,990/yr — xaloqi.com*
*Runtime: GPL v2 · Examples: Apache 2.0 · Tools + IDE + GUI: Commercial*
