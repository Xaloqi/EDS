# Integration Guide

## Xaloqi EDS — Zephyr RTOS and FreeRTOS

| Field | Value |
|---|---|
| Stack version | 1.3.0 |
| Zephyr version | v3.7.0 (pinned in `west.yml`) |
| FreeRTOS version | FreeRTOS-Kernel (any recent release; tested with HEAD) |
| ISO standard | ISO 14229-1:2020 (UDS), ISO 15765-2:2016 (ISO-TP) |
| Safety target | ASIL-B candidate |
| Last updated | 2026-04-15 |

---

## Contents

1. [Supported Scope Matrix](#1-supported-scope-matrix)
2. [RAM and Flash Footprint](#2-ram-and-flash-footprint)
3. [Five Steps to Integrate into Your Zephyr ECU](#3-five-steps-to-integrate-into-your-zephyr-ecu)
4. [FreeRTOS Integration](#freertos-integration)
5. [SafeBoot — MCUboot DFU over UDS](#safeboot)
6. [Board Compatibility Matrix](#6-board-compatibility-matrix)

---

## 1. Supported Scope Matrix

### 1.1 UDS Services (ISO 14229-1)

The following table covers every service defined in ISO 14229-1:2020. "Implemented" means the service handler exists, is registered in the dispatch table, and is exercised by the firmware integration test suite. "Out of scope" means the service is not compiled — requests return NRC 0x11 (serviceNotSupported).

| SID | Service Name | Status | Sub-functions / Notes |
|-----|---|---|---|
| 0x10 | DiagnosticSessionControl | **Implemented** | Sub-fn 0x01 Default, 0x02 Programming, 0x03 Extended, 0x04 SafetySystem. suppressPosRspMsgIndicationBit (bit 7) honoured. P2/P2\* timing encoded per ISO §7.2 (P2\* field = ms/10). |
| 0x11 | ECUReset | **Implemented** | Sub-fn 0x01 hardReset, 0x02 keyOffOnReset, 0x03 softReset. All three map to `sys_reboot()` on Zephyr. suppressPosRspMsgIndicationBit honoured. |
| 0x14 | ClearDiagnosticInformation | **Implemented** | groupOfDTC 0xFFFFFF (all groups) and any 3-byte DTC group code. NVM-backed status cleared via `dtc_mirror_clear_all()`. |
| 0x19 | ReadDTCInformation | **Implemented** | Sub-fn 0x01 reportNumberOfDTCByStatusMask, 0x02 reportDTCByStatusMask, 0x03 reportDTCSnapshotIdentification (returns empty identifier list — no freeze-frame data stored), 0x04 reportDTCSnapshotRecordByDTCNumber (returns empty record — no snapshot data stored), 0x06 reportDTCExtDataRecordByDTCNumber (returns empty record), 0x0A reportSupportedDTCs. All 8 DTC status bits supported (availability mask 0xFF). ISO 14229-1 DTC format (format ID 0x01). |
| 0x22 | ReadDataByIdentifier | **Implemented** | Multi-DID requests (multiple DID pairs in one request) fully supported. ASIL-B 5-step safety wrapper enforced per DID. Max 64 DIDs configurable. |
| 0x27 | SecurityAccess | **Implemented** | Odd sub-function = requestSeed, even = sendKey. Levels 1 and 2 active (0x01/0x02 and 0x03/0x04 sub-functions). AES-128-CMAC key derivation (RFC 4493). 8-byte seed with embedded sequence counter (replay protection). Lockout after configurable failed attempts. Key injection via `uds_security_algo_set_level_key()`. |
| 0x28 | CommunicationControl | **Implemented** | Sub-fn 0x00 enableRxAndTx, 0x01 enableRxAndDisableTx, 0x02 disableRxAndEnableTx, 0x03 disableRxAndTx. communicationType byte: 0x01 normalCommunication, 0x02 nmCommunication, 0x03 both. State is reset to enabled on return to Default Session. |
| 0x2E | WriteDataByIdentifier | **Implemented** | ASIL-B 5-step safety wrapper enforced per DID. Data length validated against DID definition. |
| 0x31 | RoutineControl | **Implemented** | Sub-fn 0x01 startRoutine, 0x02 stopRoutine (optional per routine descriptor), 0x03 requestRoutineResults (optional). Session and security level enforced per-routine via `routine_entry_t.min_session` and `.security_level`. Routine option record forwarded to callback. Status record (0–64 bytes) appended to positive response. |
| 0x34 | RequestDownload | **Implemented** | Programming session + Level 1 security required (enforced by ACL table). Validates `dataFormatIdentifier` (0x00 only — no compression/encryption), parses `addressAndLengthFormatIdentifier` (1–4 byte address and size fields), validates target address range against the registered flash memory map (`uds_flash_ops_t`), erases flash, and initialises the block-transfer state machine. Returns `maxNumberOfBlockLength`. **Requires** `uds_flash_ops_register()` before the first 0x34 request (see Step 5 integration note). |
| 0x36 | TransferData | **Implemented** | Programming session required. Validates block sequence counter (starts at 0x01, wraps 0xFF → 0x01 per REQ-DL-001 — counter 0x00 always rejected with NRC 0x73). Accumulates payload bytes in a write buffer, flushes full chunks to flash via `flash_write_cb`, and maintains a running CRC-32 accumulator. |
| 0x37 | RequestTransferExit | **Implemented** | Programming session required. Flushes any remaining write-buffer bytes, optionally validates an optional 4-byte CRC-32 `transferRequestParameterRecord` (NRC 0x72 on mismatch), invokes `flash_verify_cb`, and resets the transfer state machine to IDLE. Enforces `bytes_remaining == 0` before accepting exit (NRC 0x31 if incomplete). |
| 0x38 | RequestFileTransfer | **Out of scope** | — |
| 0x3D | WriteMemoryByAddress | **Out of scope** | — |
| 0x3E | TesterPresent | **Implemented** | Sub-fn 0x00 only (bits 6:0 must be 0x00). suppressPosRspMsgIndicationBit (bit 7) honoured. Resets S3Server session timeout. |
| 0x85 | ControlDTCSetting | **Implemented** | Sub-fn 0x01 DTCSettingOn, 0x02 DTCSettingOff. dtcSettingControlOptionRecord bytes accepted but ignored (state is global). State auto-reset on return to Default Session. |
| 0x86 | ResponseOnEvent | **Out of scope** | — |
| 0x87 | LinkControl | **Out of scope** | — |

**Services not listed** (e.g. 0x23 ReadMemoryByAddress, 0x24 ReadScalingDataByIdentifier, 0x29 Authentication, 0x2A ReadDataByPeriodicIdentifier, 0x2C DynamicallyDefineDataIdentifier, 0x2F InputOutputControlByIdentifier) are all out of scope and return NRC 0x11.

---

### 1.2 ISO-TP Transport (ISO 15765-2)

| Feature | Status | Notes |
|---|---|---|
| Single Frame (SF) | **Implemented** | Max 7 bytes payload. |
| First Frame (FF) | **Implemented** | Total length up to 4095 bytes. |
| Consecutive Frame (CF) | **Implemented** | Sequence number validated (0x1–0xF, wraps). |
| Flow Control (FC) | **Implemented** | CTS (FS=0), Wait (FS=1) and Overflow (FS=2) all handled. BlockSize and STmin extracted from FC and applied. |
| STmin 0x00–0x7F | **Implemented** | 0–127 ms inter-frame delay (1 ms resolution). |
| STmin 0xF1–0xF9 | **Implemented** | 100–900 µs; rounded up to 1 ms tick interval. |
| STmin 0x80–0xF0, 0xFA–0xFF | **Implemented** | Reserved values treated as 0 ms (CTS-immediate). |
| Normal addressing (11-bit CAN ID) | **Implemented** | Default: RX=0x7DF (functional), TX=0x7E8 (physical). Configurable in `diagnostics_config.yaml`. |
| Physical addressing | **Partial** | The Zephyr CAN RX filter in `zephyr_can.c` installs one filter for 0x7DF. Adding a second filter for a physical address (e.g. 0x7E0) requires one additional `can_add_rx_filter_msgq()` call — see `platform/zephyr/zephyr_can.c` line 271 for the comment. |
| Extended addressing (29-bit) | **Out of scope** | `is_extended_id` field exists in `uds_can_frame_t` but 29-bit addressing is not tested and not officially supported. |
| Mixed addressing | **Out of scope** | — |
| CAN FD (ISO 15765-2 §9.8) | **Out of scope** | `is_fd` field exists in `uds_can_frame_t`. The transport layer does not use FD frames or the FD escape sequence for PDU length > 4095 bytes. |
| Max PDU length | **4095 bytes** | Defined by `UDS_MAX_PAYLOAD_LEN` in `core/uds_types.h`. Reducible at compile time to save RAM (see §2). |

---

### 1.3 Configuration Limits

| Parameter | Default | Maximum | How to change |
|---|---|---|---|
| DID count | Per YAML | 64 (ASIL-B) | `UDS_MAX_DID_COUNT` in `uds_types.h`; ASIL-B enforces ≤ 64 |
| DTC count | Per YAML | 128 | `UDS_MAX_DTC_COUNT` in `uds_types.h` |
| Max DID data length | Per YAML | 64 bytes (ASIL-B) | `ASIL_B_MAX_DID_DATA_LEN` in `codegen.py`; non-ASIL-B builds allow up to 4095 bytes |
| Max PDU length | 4095 bytes | 4095 bytes | `UDS_MAX_PAYLOAD_LEN` in `uds_types.h` — reducing this saves RAM directly |
| Security levels | 2 | 2 | `ALGO_MAX_LEVELS` in `uds_security_algo.c` |
| P2 server max | 25 ms | Any uint16 | `timing.p2_server_max_ms` in `diagnostics_config.yaml` |
| P2\* server max | 5000 ms | Any uint16 | `timing.p2_star_server_max_ms` in YAML |
| S3 server timeout | 5000 ms | Any uint32 | `timing.s3_server_timeout_ms` in YAML |
| Diagnostics task stack | 4096 bytes | Unlimited | `CONFIG_DIAG_TASK_STACK_SIZE` in `prj.conf` |

---

### 1.4 Known Limitations

- **Functional addressing only by default**: 0x7DF RX filter installed. Physical addressing requires one additional `can_add_rx_filter_msgq()` call — not a code change, only a configuration extension.
- **Single-ECU only**: no gateway routing. Multi-ECU addressing (ISO 15765-3 normal fixed addressing to 0x7Ex/0x18DAxxxx) is not implemented.
- **CAN FD not tested**: CAN FD hardware compilation succeeds (STM32H7/G4 FDCAN driver) but the ISO-TP layer does not use FD frames. Classical CAN (max 8 bytes/frame) is the tested transport.
- **No DoIP** (ISO 13400): CAN-only. Ethernet/DoIP transport is on the roadmap.
- **Security levels fixed at 2**: AES-CMAC keys for levels 1 and 2. More levels require changing `ALGO_MAX_LEVELS`.
- **SID 0x19 — ReadDTCInformation: partial sub-function coverage.** The following sub-functions are implemented: `0x01` (reportNumberOfDTCByStatusMask), `0x02` (reportDTCByStatusMask), `0x04` (reportDTCSnapshotRecordByDTCNumber), `0x06` (reportDTCExtDataRecordByDTCNumber), `0x0A` (reportSupportedDTCs). The following sub-functions are **not implemented** and return NRC `0x12` (subFunctionNotSupported): `0x0B` (reportDTCWithPermanentStatus) and `0x19` (reportDTCExtDataRecordByRecordNumber). These are required by some OEM tool profiles (notably CANdelaStudio extended sessions). If your tester sends `0x19 0x0B` or `0x19 0x19`, the ECU will respond with NRC `0x12` — this is the correct ISO 14229-1 behaviour for unsupported sub-functions and will not cause a protocol error. Implementation of these sub-functions is planned for a future release.

---

## 2. RAM and Flash Footprint

All measurements are from **host GCC compilation** (`-O0 -std=c11`) of the production stack sources without Zephyr. The Zephyr cross-compiler (`arm-zephyr-eabi-gcc -O2`) produces smaller code; the figures below are conservative upper bounds.

### 2.1 RAM — Static Allocations

These are `bss` / `data` objects that exist for the lifetime of the ECU:

| Object | Type | Size | Notes |
|---|---|---|---|
| `s_req_buf` | `uds_msg_buf_t` | **4 098 B** | UDS request buffer. `data[4095]` + 2 B length + 1 B padding. |
| `s_resp_buf` | `uds_msg_buf_t` | **4 098 B** | UDS response buffer. |
| `uds_server_ctx_t` | Service dispatch context | **8 272 B** | Includes service table pointer array and ACL state. |
| `uds_session_ctx_t` | Session state machine | **32 B** | Current session, S3server counter. |
| `uds_security_ctx_t` | Security state machine | **64 B** | Attempt counter, pending seed, lockout timer. |
| `isotp_ctx_t` | ISO-TP state + rx_buf | **4 168 B** | Includes `rx_buf[4095]` reassembly buffer. |
| **Subtotal (BSS)** | | **~20 732 B** | **~20 KB** |

### 2.2 RAM — Stack

| Object | Size | Notes |
|---|---|---|
| Diagnostics task stack | **4 096 B** | `CONFIG_DIAG_TASK_STACK_SIZE` in `prj.conf`. |
| Worst-case stack depth | **~1 600 B** | Measured with `-fstack-usage`: path is `on_isotp_rx_complete → uds_server_process_request → service_0x22_handler → s_did_safe_read`. Gives 2.5× safety margin on a 4096-byte stack. |

### 2.3 Total RAM Summary

| Scenario | RAM |
|---|---|
| Default (4095 B max PDU) | **~24 KB** |
| Reduced PDU (512 B max PDU) | **~6 KB** *(reduce `UDS_MAX_PAYLOAD_LEN` to 512 in `uds_types.h`)* |
| Reduced PDU (256 B max PDU) | **~4 KB** |

> **Tip**: The two `uds_msg_buf_t` buffers and the `isotp_ctx_t` rx_buf account for ~12 KB of the default total. If your ECU's largest DID response is under 512 bytes (which covers all ISO 14229-1 Annex F standard DIDs and most sensor DIDs), set `UDS_MAX_PAYLOAD_LEN 512` to reduce RAM by ~18 KB at zero functional cost.

### 2.4 Flash

Flash consumption depends heavily on compiler optimisation level, LTO settings, and which Zephyr subsystems are enabled. The following figures are from the harness binary (GCC -O0, host x86-64) and a representative Zephyr cross-compile estimate:

| Component | Host binary (GCC -O0) | Zephyr ARM (estimated, -O2) |
|---|---|---|
| UDS stack `.text` | ~55 KB | ~25–35 KB |
| ISO-TP transport `.text` | ~6 KB | ~3–5 KB |
| Platform port (Zephyr) | ~5 KB (POSIX stubs) | ~8 KB (Zephyr APIs) |
| Generated code (5 DIDs) | ~4 KB | ~2 KB |
| **Total stack flash** | **~70 KB** | **~38–50 KB** |

> **Note**: The harness binary `.text` section is 88 KB; this includes the test harness, POSIX platform shims, and the full integration test runner — not representative of the production Zephyr build. For a production Zephyr ECU with `CONFIG_SIZE_OPTIMIZATIONS=y` and `CONFIG_LTO=y`, expect the stack to occupy 38–50 KB flash.

---

## 3. Five Steps to Integrate into Your Zephyr ECU

This section assumes you have an existing Zephyr application that builds successfully. The integration adds the diagnostics stack as a source-only module — no separate library, no shared library versioning, no ABI compatibility concerns.

### Step 1 — Add EDS to Your West Workspace

In your project's `west.yml`, add EDS as a module:

```yaml
manifest:
  projects:
    # ... your existing projects ...

    - name: embedded-diagnostics-suite
      url: https://github.com/your-org/embedded-diagnostics-suite
      revision: v1.3.0            # pin to a release tag
      path: eds
```

Then fetch:

```bash
west update
```

Alternatively, without west: clone the repository and set `EDS_ROOT` to its path in your `CMakeLists.txt`.

---

### Step 2 — Create Your Diagnostics Configuration

Create `your_ecu/diagnostics_config.yaml`. Start from the example most similar to your domain:

- **EV / battery development** → start from `examples/bms_ecu/diagnostics_config.yaml` (24 DIDs, BMS-specific address map, OV/UV threshold write with range validation)
- **I/O controller / gateway / ARDEP** → start from `examples/ardep_ecu/diagnostics_config.yaml` (35 DIDs, PowerIO, CAN/LIN status, calibration, DFU)
- **Minimal / generic ECU** → start from `examples/basic_ecu/diagnostics_config.yaml` (5 DIDs)

Minimal structure:

```yaml
metadata:
  ecu_name:    "YourECU"
  version:     "1.1.0"

timing:
  p2_server_max_ms:       25
  p2_star_server_max_ms:  5000
  s3_server_timeout_ms:   5000

dids:
  - id:   "0xF190"
    name: "VehicleIdentificationNumber"
    access: [read]
    min_session: "default"
    read_security_level:  0
    write_security_level: 1
    data_length: 17

  # ... add your ECU-specific DIDs ...

dtcs:
  - code:        "0xC00100"
    description: "CAN bus communication loss"
    severity:    "check_at_next_halt"

  # ... add your ECU-specific DTCs ...
```

**Generate C sources and test suite:**

```bash
python3 eds/tools/codegen.py \
    --config  your_ecu/diagnostics_config.yaml \
    --out     your_ecu/generated/ \
    --safety-wrappers \
    --asil-level B \
    --test-gen
```

This produces under `your_ecu/generated/`:
- `generated_config.h` — CAN IDs, timing constants, DID/DTC counts
- `did_handlers.h/.c` — one handler stub per DID (fill with your sensor reads)
- `did_safety_wrappers.h/.c` — ASIL-B 5-step validation before every DID access
- `uds_init.h/.c` — full stack wiring: databases → session → security → server → ISO-TP
- `safety_config.h` — compile-time ASIL-B assertions
- `tests/` — pytest integration tests (simulator + firmware-backed)

---

### Step 3 — Wire into CMakeLists.txt

In your application's `CMakeLists.txt`, add the EDS sources. If using west, reference the module path; otherwise use an absolute `EDS_ROOT`:

```cmake
# Point to the EDS repository root
set(EDS_ROOT "${ZEPHYR_BASE}/../eds")   # adjust path for your workspace layout

# Include paths
target_include_directories(app PRIVATE
    ${EDS_ROOT}/core
    ${EDS_ROOT}/core/uds_services
    ${EDS_ROOT}/transport
    ${EDS_ROOT}/config
    ${EDS_ROOT}/platform
    ${EDS_ROOT}/platform/zephyr
    ${CMAKE_CURRENT_SOURCE_DIR}/generated   # your generated/ directory
)

# Stack sources
target_sources(app PRIVATE
    # UDS core
    ${EDS_ROOT}/core/uds_server.c
    ${EDS_ROOT}/core/uds_session.c
    ${EDS_ROOT}/core/uds_security.c
    ${EDS_ROOT}/core/uds_safety.c
    ${EDS_ROOT}/core/uds_security_algo.c
    ${EDS_ROOT}/core/uds_aes_cmac.c
    ${EDS_ROOT}/core/uds_access_table.c
    ${EDS_ROOT}/core/uds_comm_control.c
    ${EDS_ROOT}/core/uds_session_stats.c
    ${EDS_ROOT}/core/uds_security_nvm.c

    # UDS service handlers
    ${EDS_ROOT}/core/uds_services/service_registration.c
    ${EDS_ROOT}/core/uds_services/service_0x10.c
    ${EDS_ROOT}/core/uds_services/service_0x11.c
    ${EDS_ROOT}/core/uds_services/service_0x14.c
    ${EDS_ROOT}/core/uds_services/service_0x19.c
    ${EDS_ROOT}/core/uds_services/service_0x22.c
    ${EDS_ROOT}/core/uds_services/service_0x27.c
    ${EDS_ROOT}/core/uds_services/service_0x28.c
    ${EDS_ROOT}/core/uds_services/service_0x2E.c
    ${EDS_ROOT}/core/uds_services/service_0x31.c
    ${EDS_ROOT}/core/uds_services/service_0x34.c
    ${EDS_ROOT}/core/uds_services/service_0x36.c
    ${EDS_ROOT}/core/uds_services/service_0x37.c
    ${EDS_ROOT}/core/uds_services/service_0x3E.c
    ${EDS_ROOT}/core/uds_services/service_0x85.c

    # ISO-TP / CAN transport
    ${EDS_ROOT}/transport/isotp.c
    ${EDS_ROOT}/transport/can_transport.c
    ${EDS_ROOT}/platform/zephyr/zephyr_can.c

    # Databases
    ${EDS_ROOT}/config/did_database.c
    ${EDS_ROOT}/config/dtc_database.c
    ${EDS_ROOT}/config/dtc_mirror.c
    ${EDS_ROOT}/config/routine_database.c

    # DFU / firmware download support
    ${EDS_ROOT}/core/uds_transfer_ctx.c
    ${EDS_ROOT}/platform/uds_flash_ops.c
    # Platform flash driver — choose ONE:
    # ${EDS_ROOT}/platform/zephyr_flash_ops.c   # MCUboot secondary-slot (recommended)
    # Or implement your own flash_ops_t and call uds_flash_ops_register() in main.c.

    # Zephyr platform
    ${EDS_ROOT}/platform/zephyr/zephyr_port.c
    ${EDS_ROOT}/platform/zephyr/zephyr_mutex.c
    ${EDS_ROOT}/platform/zephyr/zephyr_timer.c
    ${EDS_ROOT}/platform/zephyr/zephyr_wdt.c
    ${EDS_ROOT}/platform/zephyr/zephyr_platform_api.c

    # Generated sources
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/did_handlers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/did_safety_wrappers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/routine_handlers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/uds_init.c
)

# NVM store: real Zephyr NVS on hardware, RAM mock on native_sim
if(CONFIG_BOARD_NATIVE_SIM)
    target_sources(app PRIVATE ${EDS_ROOT}/platform/zephyr/nvm_store_mock.c)
else()
    target_sources(app PRIVATE ${EDS_ROOT}/platform/zephyr/nvm_store.c)
endif()
```

---

### Step 4 — Add the CAN Alias and Kconfig to Your Board Files

**Device Tree overlay** — map `can0` to your board's CAN controller:

```dts
/* boards/your_board/your_board.overlay  */
/ {
    aliases {
        can0 = &fdcan1;   /* adjust node label for your SoC */
    };
};

&fdcan1 {
    status     = "okay";
    bus-speed  = <500000>;   /* ISO 15765-4 default */
    sample-point = <875>;    /* CiA 601: 87.5% */
    can-transceiver {
        max-bitrate = <1000000>;
    };
};
```

**Kconfig** — add to your `boards/your_board/your_board.conf` or `prj.conf`:

```kconfig
# CAN driver (adjust for your SoC family)
CONFIG_CAN=y
CONFIG_CAN_INIT_PRIORITY=80
CONFIG_STATS=y
CONFIG_CAN_STATS=y

# 1 ms tick (required for ISO-TP timer accuracy)
CONFIG_SYS_CLOCK_TICKS_PER_SEC=1000
CONFIG_TICKLESS_KERNEL=n

# NVS flash (for NVM-backed DTC persistence and security counters)
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_NVS=y

# Diagnostics task stack
CONFIG_DIAG_TASK_STACK_SIZE=4096   # increase if using large DIDs or deep call chains

# Safety hardening (recommended)
CONFIG_ASSERT=y
CONFIG_STACK_CANARIES=y
CONFIG_STACK_SENTINEL=y

# No heap (ASIL-B / MISRA-21.3 compliance)
CONFIG_HEAP_MEM_POOL_SIZE=0
```

---

### Step 5 — Initialize the Stack in Your Application

Copy the pattern from `examples/basic_ecu/src/main.c`. The minimum integration is:

```c
#include "uds_init.h"
#include "generated_config.h"
#include "uds_security_algo.h"
#include <zephyr/drivers/can.h>

#define DIAG_CAN_DEV   DEVICE_DT_GET(DT_ALIAS(can0))
#define DIAG_RX_ID     GEN_CAN_RX_ID   /* 0x7DF */
#define DIAG_TX_ID     GEN_CAN_TX_ID   /* 0x7E8 */

/* 1. Initialize the platform CAN transport */
const zephyr_port_cfg_t port_cfg = { .can_dev = DIAG_CAN_DEV };
can_transport_t *can;
zephyr_port_init(&port_cfg, &can);

/* 2. (Production) Inject OEM AES-128 keys before stack init */
uint8_t key_l1[16] = { /* load from OTP / NVM */ };
uds_security_algo_set_level_key(0x01U, key_l1);
memset(key_l1, 0, sizeof(key_l1));

/* 3. Initialize the full stack (session, security, services, ISO-TP) */
uds_generated_init(can, DIAG_RX_ID, DIAG_TX_ID);

/* 4. Get context pointers for the poll loop */
uds_server_ctx_t *srv = uds_generated_get_server();
isotp_ctx_t      *tp  = uds_generated_get_isotp();

/* 5. Run the 1 ms poll loop (in a dedicated thread — see threading_guide.md) */
while (true) {
    uds_can_frame_t frame; bool ready;
    can_transport_receive(can, &frame, &ready);
    if (ready) isotp_process_rx_frame(tp, &frame, on_rx_complete, srv);
    isotp_tick_1ms(tp);
    uds_server_tick_1ms(srv);
    k_sleep(K_MSEC(1));
}
```

**Implement your DID handlers** — open `generated/did_handlers.c` and replace each stub body with a real sensor read:

```c
/* Generated stub — replace with real sensor read: */
uds_status_t your_ecu_read_coolanttemperature(uint8_t *buf)
{
    int32_t temp_raw;
    adc_channel_read(ADC_CH_COOLANT, &temp_raw);
    uint8_t encoded = (uint8_t)(temp_raw + 40);   /* offset encoding */
    buf[0] = encoded;
    return UDS_STATUS_OK;
}
```

The ASIL-B safety wrappers in `generated/did_safety_wrappers.c` call your handler only after passing the 5-step validation chain (DID exists → session valid → security level met → access permission → length matches). You do not need to add your own guards.

**Firmware download (SID 0x34/0x36/0x37) — SafeBoot:**

The simplest way to enable DFU is to add `safeboot: enabled: true` to your `diagnostics_config.yaml` and regenerate:

```yaml
safeboot:
  enabled: true
  platform: zephyr
  max_block_length: 256
```

Codegen then generates `zephyr_flash_ops_init()` automatically into `uds_init.c`. Add `platform/zephyr/zephyr_flash_ops.c` to your CMakeLists sources. No other application code changes required.

See the `examples/safeboot_ecu/` example and [`docs/INTEGRATION_GUIDE.md` — SafeBoot section](#safeboot) for the full DFU sequence, Python flash script, and MCUboot setup requirements.

**Manual flash ops registration (advanced / custom flash backend):**

If you need a custom flash backend (non-MCUboot, external SPI flash, encrypted images), implement the `uds_flash_ops_t` interface declared in `platform/uds_flash_ops.h` and call `uds_flash_ops_register()` directly before `uds_generated_init()`:

```c
#include "zephyr_flash_ops.h"

/* Call this before uds_generated_init(). */
/* Registers the MCUboot secondary-slot erase/write/verify callbacks. */
zephyr_flash_ops_init();
```

Without flash ops registered (and without `safeboot.enabled: true`), any `0x34 RequestDownload` is rejected with NRC 0x22 — this is intentional and safe.

---

## 4. FreeRTOS Integration {#freertos-integration}

This section covers everything needed to run EDS on a FreeRTOS target. The same YAML configuration, the same codegen, and the same 14 UDS services work identically — only the platform layer differs.

### 4.1 Overview

On FreeRTOS, EDS uses a callback-based integration model. You provide:
- A **CAN send function** — called when the stack needs to transmit a frame
- An optional **NVM ops table** — for persistent DTC/security counter storage
- An optional **reset callback** — for PMIC or custom power sequencing

You wire:
- Your **CAN RX interrupt** to call `eds_platform_can_input()` whenever a diagnostic frame arrives

Everything else (ISO-TP assembly, UDS dispatch, session management, security, DTC persistence) is handled by the stack internally.

### 4.2 Five-Step Integration

#### Step 1 — Implement your CAN send function

```c
#include "platform_api.h"

/* Called by the EDS stack when it needs to transmit a CAN frame.
 * Replace the body with your MCU's CAN peripheral call. */
static uds_status_t my_can_send(const eds_can_frame_t *frame)
{
    /* STM32 HAL example: */
    CAN_TxHeaderTypeDef hdr = {
        .StdId = frame->id,
        .DLC   = frame->dlc,
        .IDE   = CAN_ID_STD,
        .RTR   = CAN_RTR_DATA,
    };
    uint32_t mailbox;
    if (HAL_CAN_AddTxMessage(&hcan1, &hdr, frame->data, &mailbox) != HAL_OK) {
        return UDS_STATUS_ERR_CAN_TX_FAILED;
    }
    return UDS_STATUS_OK;
}
```

#### Step 2 — Initialise the EDS platform layer

```c
#include "platform_api.h"
#include "freertos_can.h"

void app_init(void)
{
    uds_status_t rc = eds_platform_init(&(eds_platform_cfg_t){
        .can_send            = my_can_send,
        /* Optional: provide NVM ops for persistent DTC + security counters.
         * If omitted, a RAM stub is used (data lost on reset). */
        .nvm = {
            .read     = my_flash_read,
            .write    = my_flash_write,
            .is_ready = my_flash_is_ready,
        },
        .uds_task_stack_size = 2048U,
        .uds_task_priority   = 5U,
    });
    configASSERT(rc == UDS_STATUS_OK);
}
```

#### Step 3 — Initialise the UDS stack

```c
#include "uds_init.h"
#include "generated_config.h"
#include "freertos_can.h"

    /* Identical call to the Zephyr integration. */
    uds_status_t rc = uds_generated_init(
        freertos_can_get_transport(),
        GEN_CAN_RX_ID,   /* 0x7DF */
        GEN_CAN_TX_ID    /* 0x7E8 */
    );
    configASSERT(rc == UDS_STATUS_OK);
```

#### Step 4 — Start the UDS poll task

`eds_freertos_start()` creates the UDS poll task using static allocation (no heap). It encapsulates the poll loop, ISO-TP RX completion callback, and static buffer storage — there is nothing to copy.

```c
#include "platform_api.h"

    uds_status_t rc = eds_freertos_start();
    configASSERT(rc == UDS_STATUS_OK);
```

The poll task:
- Runs every 1 ms (`vTaskDelay(1)` — requires `configTICK_RATE_HZ=1000`)
- Drains the CAN RX queue and feeds frames to the ISO-TP reassembler
- Calls `isotp_tick_1ms()` and `uds_server_tick_1ms()` each iteration
- On ISO-TP RX completion, dispatches to `uds_server_process_request()` and transmits the response

#### Step 5 — Start the FreeRTOS scheduler

```c
    vTaskStartScheduler();   /* does not return */
```

**Complete minimal `main()`:**

```c
int main(void)
{
    eds_platform_init(&(eds_platform_cfg_t){
        .can_send            = my_can_send,
        .uds_task_stack_size = 2048U,
        .uds_task_priority   = 5U,
    });

    uds_generated_init(
        freertos_can_get_transport(),
        GEN_CAN_RX_ID,
        GEN_CAN_TX_ID);

    eds_freertos_start();

    vTaskStartScheduler();

    for (;;) { }   /* unreachable */
}
```

Call `eds_platform_can_input()` from your CAN RX interrupt handler or callback whenever a frame matching a diagnostic CAN ID arrives. The function is ISR-safe — it posts to an internal queue without calling the UDS stack directly.

```c
/* STM32 HAL example — CAN RX interrupt callback */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    CAN_RxHeaderTypeDef hdr;
    uint8_t             data[8];

    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &hdr, data) != HAL_OK) {
        return;
    }

    eds_can_frame_t frame = {
        .id          = hdr.StdId,
        .dlc         = (uint8_t)hdr.DLC,
        .is_extended = false,
    };
    memcpy(frame.data, data, hdr.DLC);

    /* Thread-safe, ISR-safe. */
    eds_platform_can_input(&frame);
}
```

**CAN ID filtering:** Only pass frames with IDs `0x7DF` (ISO 15765-4 functional broadcast) and your ECU's physical RX ID (e.g. `0x7E0`) to `eds_platform_can_input()`. Configure your CAN controller's hardware acceptance filter accordingly. Passing non-diagnostic frames is harmless but wastes queue space.

### 4.3 ECU reset — TX confirmation contract

`eds_platform_ecu_reset()` triggers an immediate hardware reset. The caller is responsible for ensuring the positive `0x51` response has been fully transmitted over CAN before calling this function. The correct sequence is:

```c
/* 1. Transmit the positive response. */
isotp_transmit(tp, resp.data, resp.length);

/* 2. Wait for the ISO-TP TX state machine to return to IDLE.
 *    This confirms the last CAN frame has left the TX mailbox
 *    and the N_As timer has not expired. */
isotp_state_t rx_st, tx_st;
uint32_t wait_ms = 0U;
do {
    vTaskDelay(1);
    isotp_tick_1ms(tp);
    isotp_get_state(tp, &rx_st, &tx_st);
    wait_ms++;
} while ((tx_st != ISOTP_STATE_IDLE) && (wait_ms < 50U));  /* 50ms max */

/* 3. Flush NVM (DTC mirror, session stats, lifecycle counter). */
eds_platform_nvm_flush();

/* 4. Reset. Does not return on success. */
eds_platform_ecu_reset(reset_type);
```

**Do not use a fixed `vTaskDelay()` as a substitute for step 2.** A fixed delay is unreliable: it may expire before the CAN peripheral completes frame transmission on a loaded bus (ISO 15765-2 N_As = 25 ms), and it wastes time on an idle bus. The ISO-TP TX pump is the correct confirmation mechanism.

### 4.4 Optional: custom reset callback

By default, `eds_platform_ecu_reset()` writes to the ARM Cortex-M SCB AIRCR register (works on all Cortex-M targets). If your platform requires PMIC control, a GPIO power latch, or a watchdog-triggered reset, register a custom callback before `eds_platform_init()`:

```c
static void my_reset_callback(uint8_t reset_type)
{
    /* reset_type: 0x01 hardReset, 0x02 keyOffOnReset, 0x03 softReset */
    my_pmic_shutdown(reset_type);   /* must not return */
}

/* Call before eds_platform_init(). */
eds_platform_set_reset_cb(my_reset_callback);
```

### 4.5 CMake build

Use the provided toolchain file to cross-compile. It handles the bare-metal probe issue that causes CMake to fail with `cannot find crt0.o` when using `arm-none-eabi-gcc` without a sysroot.

```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/arm-none-eabi.cmake \
  -DEDS_PLATFORM=freertos \
  -DFREERTOS_DIR=/path/to/FreeRTOS-Kernel \
  -DBOARD=qemu_cortex_m4 \
  -GNinja \
  examples/basic_ecu_freertos

ninja -C build
```

For custom boards, pass `-DFREERTOS_CONFIG_DIR=<dir containing FreeRTOSConfig.h>`. The key `FreeRTOSConfig.h` requirement for EDS is `configTICK_RATE_HZ=1000` — ISO-TP timing requires 1 ms tick resolution.

### 4.6 NVM ops for persistent storage

If you omit `.nvm` in `eds_platform_cfg_t`, EDS uses a RAM-backed stub. DTC fault history, security attempt counters, and session statistics are lost on every reset. For production targets, provide a flash-backed implementation:

```c
/* Minimal interface — key is a uint16_t (NVM_KEY_* from nvm_store.h).
 * Max record size: NVM_MAX_RECORD_BYTES (512 bytes).
 * Any key-value flash driver works — the EDS stack never erases the
 * underlying flash directly. */

static uds_status_t my_nvm_read(uint16_t key, uint8_t *buf,
                                 size_t len, size_t *out_len)
{
    return my_kvstore_read(key, buf, len, out_len);
}

static uds_status_t my_nvm_write(uint16_t key, const uint8_t *buf, size_t len)
{
    return my_kvstore_write(key, buf, len);
}

static bool my_nvm_is_ready(void)
{
    return my_kvstore_is_initialized();
}
```

Populate all three pointers in `.nvm` — if any is NULL, the RAM stub is used instead.

### 4.7 FreeRTOSConfig.h requirements

| Setting | Required value | Reason |
|---|---|---|
| `configTICK_RATE_HZ` | `1000` | ISO-TP N_Cr/N_As/N_Bs timing needs 1 ms resolution |
| `configSUPPORT_STATIC_ALLOCATION` | `1` | EDS uses static queue and task allocation (no heap) |
| `configUSE_MUTEXES` | `1` | Required by FreeRTOS queue internals |
| `configUSE_COUNTING_SEMAPHORES` | `1` | Required by `xQueueSendFromISR` path |

A minimal `FreeRTOSConfig.h` for QEMU Cortex-M4 is provided at `examples/basic_ecu_freertos/boards/qemu_cortex_m4/FreeRTOSConfig.h`. Copy and adapt it for your MCU.

---

## 5. SafeBoot — MCUboot DFU over UDS {#safeboot}

SafeBoot integrates the MCUboot secondary slot with UDS download services
(0x34 RequestDownload / 0x36 TransferData / 0x37 RequestTransferExit) via
a single YAML flag. Codegen handles the wiring automatically.

### 5.1 Enable SafeBoot

Add to your `diagnostics_config.yaml`:

```yaml
safeboot:
  enabled: true
  platform: zephyr          # only zephyr supported in v1.3.0
  max_block_length: 256     # bytes per TransferData block (CAN classical: 256)
```

Regenerate:

```bash
python3 tools/codegen.py \
  --config your_ecu/diagnostics_config.yaml \
  --out    your_ecu/generated/ \
  --safety-wrappers --asil-level B --no-manifest
```

Add `platform/zephyr/zephyr_flash_ops.c` to your `CMakeLists.txt` sources.
That is the complete integration — no application code changes.

### 5.2 What codegen generates

With `safeboot.enabled: true`, `generated/uds_init.c` includes:

```c
#include "zephyr_flash_ops.h"   /* generated — only when safeboot enabled */

/* Inside uds_generated_init(): */
status = zephyr_flash_ops_init();
if (status != UDS_STATUS_OK) {
    return status;
}
```

`zephyr_flash_ops_init()` registers the MCUboot secondary-slot flash
operations (`image_1` partition). The three download services then accept
requests. No other changes to `main.c` or any other file.

### 5.3 DFU sequence

The tester tool, CI script, or production-line fixture performs these steps:

```
1.  DiagnosticSessionControl  0x10 0x02        → programming session
2.  SecurityAccess             0x27 0x01        → request seed
3.  SecurityAccess             0x27 0x02 <key>  → unlock Level 1
4.  RequestDownload            0x34 ...         → erase image_1, get block size
5.  TransferData               0x36 × N         → write firmware blocks
6.  RequestTransferExit        0x37             → verify CRC-32, accept image
7.  ECUReset                   0x11 0x01        → reboot → MCUboot swap
```

After the reset, MCUboot detects a valid image in `image_1`, copies it to
`image_0`, and boots the new firmware.

### 5.4 Safety properties

| Property | Mechanism |
|---|---|
| Programming session required | ACL table enforces session 0x02 for 0x34 |
| Security Level 1 required | ACL table enforces unlock before 0x34 |
| Address range validated | `zephyr_flash_ops.c` bounds-checks against `image_1` partition |
| CRC-32 verified | `service_0x37` reads back written bytes and checks CRC before accepting |
| Primary slot never written | `image_0` is read-only; MCUboot performs the swap |

### 5.5 Board requirements

- `CONFIG_FLASH_MAP=y`, `CONFIG_FLASH=y` in board conf
- `image_1` partition defined in board DTS (flash map)
- MCUboot installed in the primary slot before application firmware

See `examples/safeboot_ecu/boards/nucleo_h743zi/` for a complete working
board overlay and conf for the STM32 Nucleo-H743ZI2.

### 5.6 Python flash script

```python
# Minimal DFU example using udsoncan + python-can
# pip install udsoncan python-can

import can, udsoncan
from udsoncan.client import Client

bus    = can.Bus(channel="can0", interface="socketcan", bitrate=500_000)
config = {"request_timeout": 5.0, "p2_timeout": 5.0, "p2_star_timeout": 25.0}

with open("firmware.bin", "rb") as f:
    firmware = f.read()

with Client(bus, request_id=0x7DF, response_id=0x7E8, config=config) as c:
    c.change_session(udsoncan.services.DiagnosticSessionControl.Session.programmingSession)
    seed = c.request_seed(0x01).service_data.seed
    c.send_key(0x02, derive_key(seed))           # your key derivation
    result = c.request_download(
        udsoncan.MemoryLocation(0, len(firmware), 32, 32),
        dfi=udsoncan.DataFormatIdentifier(0, 0),
    )
    block_size = result.service_data.max_length
    offset, seq = 0, 1
    while offset < len(firmware):
        c.transfer_data(seq, firmware[offset:offset+block_size])
        offset += block_size; seq = (seq % 0xFF) + 1
    c.request_transfer_exit()
    c.ecu_reset(udsoncan.services.ECUReset.ResetType.hardReset)
```

See `examples/safeboot_ecu/README.md` for the complete annotated script.

---

## 6. Board Compatibility Matrix

### 6.1 Tested in CI (every commit)

| Board | Platform | Build Type | CAN Driver | Test Level |
|---|---|---|---|---|
| Linux host simulation | Zephyr `native_sim` | `ZEPHYR_TOOLCHAIN_VARIANT=host` | `CONFIG_CAN_LOOPBACK` (virtual) | Full: unit tests + firmware integration tests + simulator tests |
| ST Nucleo H743ZI | Zephyr `nucleo_h743zi` | ARM Cortex-M7 cross-compile | `st,stm32h7-fdcan` | Compile-only (no hardware in CI) |
| QEMU `mps2-an386` (Cortex-M4) | FreeRTOS | `arm-none-eabi-gcc` cross-compile | Stub loopback | Build + binary size check (`freertos-qemu` CI job) |

### 6.2 Validated by Example (not in CI)

| Board | Zephyr Identifier | Notes |
|---|---|---|
| Mercedes-Benz ARDEP | `ardep` (in ARDEP workspace) | STM32G4-series, onboard FDCAN + LIN. Overlay: `examples/ardep_ecu/boards/ardep/ardep.overlay`. Full example: `examples/ardep_ecu/`. |
| Generic BMS ECU (STM32H7 or any CAN) | `nucleo_h743zi` / any | 24-DID BMS configuration. Native_sim overlay provided. Full example: `examples/bms_ecu/`. |

### 6.3 Expected to Work — Not Tested

These boards use CAN drivers that the EDS Zephyr CAN abstraction layer (`platform/zephyr/zephyr_can.c`) is compatible with. The API calls are `can_add_rx_filter_msgq()`, `can_send()`, `can_start()` — all part of the stable Zephyr CAN API. No board-specific code exists in the EDS stack.

| Board family | Zephyr CAN driver | Notes |
|---|---|---|
| STM32H5 / STM32U5 / STM32G0 | `st,stm32h7-fdcan` | Same FDCAN driver as nucleo_h743zi. May need clock source Kconfig adjustment. |
| STM32F series (bxCAN) | `st,stm32-bxcan` | Classic bxCAN, 11-bit only, 1 Mbps max. No FD. |
| NXP S32K1xx / K3xx | `nxp,flexcan` | FlexCAN driver. Used by Eclipse OpenBSW reference platform. |
| NXP IMXRT | `nxp,flexcan` | Same driver family. |
| Nordic nRF52840 / nRF5340 | `nordic,nrf-can` (via MCP2515 SPI) | nRF52840 has no onboard CAN; requires external transceiver. |
| Renesas RA | `renesas,ra-canfd` | Renesas RA CANFD driver, Zephyr ≥ 3.6. |
| Microchip SAM E51/E54 | `atmel,sam-can` | Bosch M_CAN, same register map as STM32 FDCAN. |
| QEMU (virt) | `zephyr,can-loopback` | Same as native_sim. Useful for CI without native_posix build. |

### 6.4 Porting to a New Board

If your board is not in the list above, three things are required:

1. **CAN driver**: Your SoC must have a Zephyr CAN driver (`CONFIG_CAN=y`). Check `zephyr/drivers/can/` in your Zephyr installation.

2. **Device Tree alias**: Add `can0 = &<your_can_node>` to your board overlay. See `boards/native_sim/native_sim.overlay` or `examples/ardep_ecu/boards/ardep/ardep.overlay` for examples.

3. **Kconfig**: Enable `CONFIG_CAN=y`, `CONFIG_SYS_CLOCK_TICKS_PER_SEC=1000`, `CONFIG_NVS=y` (for DTC persistence). Add the board-specific CAN driver symbol (e.g. `CONFIG_CAN_STM32FD=y`).

No changes to the EDS stack source files are required for new board support. The CAN abstraction layer (`platform/zephyr/zephyr_can.c`) uses only the stable Zephyr CAN API.

---

## Quick Reference

### Build Commands

```bash
# Generate sources + tests from your config
python3 eds/tools/codegen.py \
    --config  your_ecu/diagnostics_config.yaml \
    --out     your_ecu/generated/ \
    --safety-wrappers --asil-level B --test-gen

# Build for simulation (no hardware)
west build -b native_sim your_ecu \
    -- -DDIAG_SKIP_CODEGEN=ON \
    -DDTC_OVERLAY_FILE=eds/boards/native_sim/native_sim.overlay \
    -DEXTRA_CONF_FILE=eds/boards/native_sim/native_sim.conf

# Build for Nucleo H743ZI
west build -b nucleo_h743zi your_ecu \
    -- -DDIAG_SKIP_CODEGEN=ON

# Build for FreeRTOS (ARM Cortex-M4 / QEMU)
cmake -B build_freertos \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain/arm-none-eabi.cmake \
    -DEDS_PLATFORM=freertos \
    -DFREERTOS_DIR=/path/to/FreeRTOS-Kernel \
    -DBOARD=qemu_cortex_m4 \
    -GNinja \
    examples/basic_ecu_freertos
ninja -C build_freertos

# Run simulator tests (no hardware required)
cd your_ecu/generated/tests
pytest . -v --can-interface=simulator

# Run firmware-backed integration tests (no hardware required)
cd eds && bash build_harness.sh --fast
cd your_ecu/generated/tests
pytest test_firmware_services.py -v
```

### Configuration Cheat Sheet

| What you want | Where to change |
|---|---|
| Add a DID | `diagnostics_config.yaml` → `dids:` → run `codegen.py` |
| Change CAN bus speed | `boards/<board>/<board>.overlay` → `bus-speed` (Zephyr) or CAN peripheral init (FreeRTOS) |
| Change CAN RX/TX IDs | `diagnostics_config.yaml` (add `can:` section) or `generated_config.h` directly |
| Inject OEM security key | `main.c`: `uds_security_algo_set_level_key(0x01U, your_key)` |
| Reduce RAM usage | `core/uds_types.h`: reduce `UDS_MAX_PAYLOAD_LEN` |
| Change max DID count | `core/uds_types.h`: `UDS_MAX_DID_COUNT` |
| Enable hardware TRNG | Zephyr: `CONFIG_ENTROPY_GENERATOR=y`; FreeRTOS: provide TRNG in `uds_security_algo_set_rng_cb()` |
| Use persistent NVM (FreeRTOS) | Provide `.nvm` ops in `eds_platform_cfg_t` — see Section 4.5 |

### Related Documentation

| Document | Location |
|---|---|
| Architecture overview | `docs/ARCHITECTURE.md` |
| MISRA C:2012 deviation log | the MISRA Deviation Log (Professional tier — xaloqi.com) |
| Safety model | `docs/Safety_Model.md` |
| ASIL-B threading model | `docs/threading_guide.md` |
| Code generation system | `docs/CODEGEN_ARCHITECTURE.md` |
| Testing strategy | `docs/TESTING_STRATEGY.md` |
| ARDEP upgrade guide | `docs/ARDEP_UPGRADE_GUIDE.md` |
| AES-CMAC security changes | `docs/PHASE1_SECURITY_CHANGES.md` |
| OEM key injection + security configuration | the Security Integration Guide (Professional tier — xaloqi.com) |
| BMS ECU example | `examples/bms_ecu/README.md` |
