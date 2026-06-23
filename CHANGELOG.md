# Changelog

All notable changes to the Xaloqi EDS are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning follows [Semantic Versioning](https://semver.org/).

---
## [Unreleased] — SID 0x19 sub-functions 0x0B and 0x19

### Added

- **SID 0x19/0x0B — `reportDTCFaultDetectionCounter`** (ISO 14229-1 §11.3.11).
  Returns a 4-byte record `[dtcHB, dtcMB, dtcLB, faultDetectionCounter]` for each
  DTC where `testFailed == 0` and `fault_detection_counter < 0xFF`. No
  `DTCStatusAvailabilityMask` byte in the response (per-spec format differs from
  0x01/0x02/0x0A). Empty list response (2-byte header) is valid and returned when no
  DTCs qualify.

- **SID 0x19/0x19 — `reportDTCWithPermanentStatus`** (ISO 14229-1 §11.3.25).
  Returns `[0x59, 0x19, availabilityMask, {dtcHB, dtcMB, dtcLB, statusByte}…]` for
  each DTC marked permanent. Permanent DTCs are not cleared by SID 0x14 — only by
  the application via `dtc_database_set_permanent(dtc_code, false)` after a
  successful drive-cycle healing sequence.

- **`dtc_entry_t` — two new fields** (`config/dtc_database.h`):
  - `uint8_t fault_detection_counter` — debounce counter managed by the application
    via `dtc_database_set_fault_counter(dtc_code, counter)`. Range 0x00–0xFE;
    0xFF is reserved (confirmed, excluded from 0x0B response). Initialised to 0x00.
  - `bool is_permanent` — managed via `dtc_database_set_permanent(dtc_code, bool)`.
    When true, SID 0x14 preserves this DTC's status byte. Initialised to false.

- **Three new `dtc_database` API functions** (`config/dtc_database.h`):
  - `dtc_database_set_fault_counter(dtc_code, counter)` — set debounce counter.
  - `dtc_database_set_permanent(dtc_code, permanent)` — mark/unmark permanent.
  - `dtc_database_clear_non_permanent()` — clear status bytes for all non-permanent
    DTCs (the production path for SID 0x14).

- **11 new unit tests** (TC-0x19-026 through TC-036) covering both sub-functions,
  the no-availability-mask format of 0x0B, the 0xFF counter exclusion rule, and the
  0x14 ↔ permanent DTC interaction.

### Changed

- **SID 0x14 now calls `dtc_database_clear_non_permanent()`** instead of
  `dtc_database_clear_all()`. Behaviour is identical when no DTCs are permanent.
  `dtc_database_clear_all()` is retained unchanged (used by test resets).

- **`dtc_database_register()`** initialises `fault_detection_counter = 0x00` and
  `is_permanent = false` for every new entry — no YAML or codegen change required.

### Fixed

- **`uds_comm_control_init()` never called in generated init sequence** — all
  `0x28` (CommunicationControl) and `0x85` (ControlDTCSetting) requests returned
  NRC 0x22 (conditionsNotCorrect) in every generated build since launch. Root cause:
  `uds_comm_control.c` gates on `s_initialized`; `uds_comm_control_init()` was only
  called in unit tests (`test_service_0x28.c`, `test_service_0x85.c`), masking the
  gap. Fix: added Step 5.8 to `uds_init.c.j2` — `uds_comm_control_init()` with NULL
  platform callbacks (state tracking works immediately; OEM integration comment
  explains how to wire real CAN-filter and DTC-gate callbacks). All 8 examples
  regenerated. Closes [#28](https://github.com/Xaloqi/EDS/issues/28).

- **`safeboot.platform: freertos` had no effect — wrong flash ops header generated**
  — `uds_init.c.j2` hardcoded `#include "zephyr_flash_ops.h"` and
  `zephyr_flash_ops_init()` in the safeboot block, ignoring the `safeboot_platform`
  context variable that `codegen.py` already passed correctly. Announced as working
  in v1.8.0 (CHANGELOG §`safeboot.platform` key) but any FreeRTOS safeboot build
  produced a compile error (`zephyr_flash_ops.h: No such file or directory`). Fix:
  template now uses `{{ safeboot_platform }}_flash_ops.h` and
  `{{ safeboot_platform }}_flash_ops_init()`. `safeboot_ecu` (Zephyr) and
  `safeboot_freertos_ecu` (FreeRTOS) both regenerated correctly.

---
## [1.8.2] — Bug fix (closes #37)

### Fixed
- All 8 example `on_isotp_rx_complete()` callbacks now handle `pending_reset_type`
  after dispatch: flush NVM, wait 50 ms for ISO-TP TX to reach the wire, then call
  `zephyr_port_ecu_reset()` / `eds_platform_ecu_reset()`. Previously the ECU sent the
  `0x11` positive response but never reset (closes #37).

---
## [1.8.1] — Bug fixes (closes #34, #35)

### Fixed

- **`platform/nvm_store.h` — header relocated to platform-neutral directory**
  (`platform/zephyr/nvm_store.h` → `platform/nvm_store.h`, closes [#34](https://github.com/Xaloqi/EDS/issues/34)).
  `nvm_store.h` is a shared interface used by both the Zephyr and FreeRTOS
  backends. Placing it inside `platform/zephyr/` caused build failures when
  integrating `freertos_nvm.c` into a custom build system without the provided
  `CMakeLists.txt`. The file already documented its intended location
  (`FILE: platform/nvm_store.h`) — it is now there. Removes the `platform/zephyr`
  workaround include from all three FreeRTOS example `CMakeLists.txt` files.

- **`campaigns/safeboot_freertos_dfu.yaml` — OTA campaign expanded to full
  ISO 14229 production sequence** (8 steps → 15 steps, closes [#35](https://github.com/Xaloqi/EDS/issues/35)).
  Previous campaign omitted `CommunicationControl` (0x28) and `ControlDTCSetting`
  (0x85) — both required by OEM production toolchains (BMW, VAG). Added:
  - Step 4: `0x28 0x03` disableRxAndTx before download
  - Step 5: `0x85 0x02` DTCSettingOff before download
  - Steps 13–15 (post-reset): extended session → `0x85 0x01` DTCSettingOn → `0x28 0x00` enableRxAndTx
  Both services have always been fully implemented in the EDS UDS stack.

---
## [1.8.0] — FreeRTOS OTA DFU example (closes #28)

### Added

- **`examples/safeboot_freertos_ecu/`** — FreeRTOS UDS OTA DFU example on
  STM32H743ZI. FreeRTOS companion to `examples/safeboot_ecu/` (Zephyr + MCUboot).
  Demonstrates the complete 0x34 / 0x36×N / 0x37 / 0x11 firmware download pipeline
  without MCUboot dependency:

  | Layer | File |
  |---|---|
  | Flash HAL | `platform/freertos/freertos_flash_ops.c` |
  | Flash interface | `platform/freertos/freertos_flash_ops.h` |
  | FreeRTOS main | `examples/safeboot_freertos_ecu/src/main.c` |
  | TestLab campaign | `examples/safeboot_freertos_ecu/campaigns/safeboot_freertos_dfu.yaml` |

  Flash layout on STM32H743ZI (2 MB dual-bank):

  | Region | Address | Size | Purpose |
  |---|---|---|---|
  | Bank 1 | 0x08000000 | 1 MB | Running application |
  | Bank 2 — OTA | 0x08100000 | 896 KB | OTA staging area |
  | Bank 2 — NVM | 0x081E0000 | 128 KB | UDS NVM (reserved) |

- **`platform/freertos/freertos_flash_ops.h/.c`** — STM32H743ZI dual-bank flash
  ops implementing `uds_flash_ops_t` (erase / write / verify).
  - Real hardware: STM32H743 HAL (`HAL_FLASHEx_Erase` / `HAL_FLASH_Program`)
    activated when `STM32H7xx` is defined.
  - CI / QEMU: RAM stub activated automatically when `STM32H7xx` is not defined.
    Compile succeeds, CRC verification works, writes are not persistent.
  - Flash driver design based on dual-bank driver contributed by chenyurong22
    (Siemens) in [#28](https://github.com/Xaloqi/EDS/issues/28). Thank you.

- **New `diagnostics_config.yaml` key**: `safeboot.platform: freertos` causes
  codegen to generate `freertos_flash_ops_init()` at Step 5.7 of `uds_init.c`
  instead of `zephyr_flash_ops_init()`.

- **New routine RID 0xFF01 — `VerifyOTASlotIntegrity`** (replaces Zephyr-specific
  `VerifyBootloaderIntegrity` in the FreeRTOS example): reads the first 8 bytes of
  Bank 2, validates that a valid ARM Cortex-M7 vector table is present (stack pointer
  in SRAM range, Reset_Handler with Thumb bit set).

- **CI job `freertos-safeboot`** — compile-only build of `safeboot_freertos_ecu/`
  against QEMU Cortex-M4 with RAM stub flash. No STM32 HAL required in CI.

### Note on bank swap

This example writes a new image to Bank 2 and verifies CRC via 0x37.
The boot-time bank switch (Bank 2 → Bank 1) is the customer's bootloader
responsibility. The EDS **Developer tier** adds the A/B swap state machine
with metadata sector, boot flag, and N-boot rollback counter.

Reported by chenyurong22 (Siemens) in [#28](https://github.com/Xaloqi/EDS/issues/28).

---
## [1.7.4] — ISO-TP TX frame padding (closes #29)

### Added

- **`ISOTP_TX_PADDING` — configurable TX frame padding** (ISO 15765-2 Annex B).
  Opt-in compile-time flag (default off — zero behaviour change for existing
  integrations). When enabled, unused bytes in all transmitted frames are
  filled with `ISOTP_TX_PADDING_BYTE` (default `0xCC`) and DLC is extended:

  | Frame | Padding off | Padding on |
  |---|---|---|
  | Classic CAN SF | DLC = length+1 | DLC = 8, tail = 0xCC |
  | CAN FD SF | DLC = length+2 | DLC = next valid FD DLC |
  | Classic CAN FF | DLC = 8 (unchanged) | DLC = 8 (already full) |
  | CAN FD FF escape | DLC = 6+data | DLC = next valid FD DLC |
  | Classic CAN CF | DLC = bytes+1 | DLC = 8, tail = 0xCC |
  | FC | DLC = 3 | DLC = 8, bytes [3..7] = 0xCC |

  Enable in Zephyr: `CONFIG_ISOTP_TX_PADDING=y`.
  Enable in FreeRTOS / bare-metal: `-DISOTP_TX_PADDING=1`.
  See [docs/ISOTP_PADDING.md](docs/ISOTP_PADDING.md) for full reference.

- **6 new unit tests** in `test_isotp_padding` suite, all passing with
  `ISOTP_TX_PADDING=1` and `ISOTP_ENABLE_CAN_FD=1`.

- `ISOTP_TX_PADDING` and `ISOTP_TX_PADDING_BYTE` added to root `Kconfig`.

- **`sbom.json`** — CycloneDX 1.4 Software Bill of Materials at repo root.
  Lists all runtime and test dependencies (Zephyr 3.7.0, FreeRTOS Kernel,
  LwIP 2.2.0, Unity test framework) with SPDX license identifiers and PURLs.

- **DoIP feature matrix** — `docs/ARCHITECTURE.md` §6.2 now contains a
  precise 18-row ISO 13400-2:2019 feature matrix with payload type codes,
  implemented/not-implemented status, and standard clause references.

- **AI-assisted development policy** — `CONTRIBUTING.md` now documents how
  Claude Code tooling is used, human review gates, CI requirements, and the
  8 safety-critical files requiring explicit sign-off. Maintainer
  responsibility statement added. DCO sign-off added to PR checklist.

Reported by chenyurong22 in [#29](https://github.com/Xaloqi/EDS/issues/29).

---
## [1.7.3] — OTA bootloader example for nucleo_h743zi

### Added — safeboot_ecu: complete UDS DFU + MCUboot pipeline on Zephyr

Closes [#24](https://github.com/Xaloqi/EDS/issues/24). The `safeboot_ecu`
example now ships as a complete, runnable OTA update reference for the
STM32H743ZI (Nucleo-H743ZI2).

The protocol side (service handlers 0x34/0x36/0x37, `zephyr_flash_ops.c`,
transfer context, generated `uds_init.c` with `zephyr_flash_ops_init()`) was
already complete since v1.7.0. This release completes the application layer
so the example compiles and runs end-to-end.

| File | Change |
|---|---|
| `src/main.c` | MCUboot image confirmation via `boot_is_img_confirmed()` / `boot_write_img_confirmed()` on first post-swap boot; non-fatal if it fails (MCUboot rollback is the correct safety net) |
| `boards/nucleo_h743zi/nucleo_h743zi.conf` | `CONFIG_BOOTLOADER_MCUBOOT=y` + `CONFIG_MCUBOOT_IMG_MANAGER=y` (required for MCUboot image manager API) |
| `generated/did_handlers.c` | All 5 DID backing stores filled: VIN `XALQ1EDS00SFBT001`, ECU serial `SFB00001`, app SW ident `v1.0.0`, active session `0x01`, supplier `XALOQI    ` |
| `generated/routine_handlers.c` | `0xFF00 CheckProgrammingPreconditions` (PASS + result cache); `0xFF01 VerifyBootloaderIntegrity` (reads `image_0`, checks MCUboot magic `0x96f3b83d`, returns 2-byte status + sub-code) |
| `README.md` | Full guide: hardware wiring (FDCAN1 PD0/PD1 + TJA1051), flash layout, build/sign/flash, TestLab campaign YAML, manual UDS byte sequence, post-DFU DID check, ASIL-B properties |

Reported in: https://github.com/Xaloqi/EDS/issues/24

---
## [1.7.2] — CAN FD platform HAL + strict session gate

### Added — CAN FD HAL for Zephyr and FreeRTOS

Completes the CAN FD story started in v1.7.1. The ISO-TP transport layer
already handled FD frames correctly; this release wires the platform HALs
so FD frames flow through on real hardware.

All changes are guarded by `ISOTP_ENABLE_CAN_FD` — Classic CAN builds
(the default) are byte-for-byte identical: `eds_can_frame_t` stays 8 bytes,
no FD code is compiled in.

| File | Change |
|------|--------|
| `platform/platform_api.h` | `EDS_CAN_FRAME_MAX_DLEN` (8 Classic / 64 FD); `eds_can_frame_t.data[]` uses it; `is_fd` field added under `#if ISOTP_ENABLE_CAN_FD` |
| `platform/zephyr/zephyr_can.c` | TX: sets `CAN_FRAME_FDF` + `can_bytes_to_dlc()`; RX: propagates `CAN_FRAME_FDF → is_fd` + `can_dlc_to_bytes()`; init: `CAN_MODE_FD` when FD enabled |
| `platform/freertos/freertos_can.c` | TX: raises DLC limit to 64; passes `is_fd` to customer send callback; RX: propagates `is_fd` instead of hardcoding `false` |

**Zephyr integration:** add `CONFIG_CAN_FD_MODE=y` to Kconfig and compile with `-DISOTP_ENABLE_CAN_FD=1`.

Reported in: https://github.com/Xaloqi/EDS/issues/16

### Added — Strict programming session gate

OEM diagnostic toolchains (BMW, VAG) require the tester to enter **Extended
Diagnostic Session before requesting Programming Session** — a direct
Default → Programming jump is rejected. This policy is now configurable
at runtime without touching codegen or YAML.

ISO 14229-1 §7.4.2.3 does not normatively prohibit Default→Programming,
so the **default remains permissive** — zero behaviour change for existing
integrations.

New API in `core/uds_session.h`:

```c
uds_status_t uds_session_set_strict_programming(
    uds_session_ctx_t *ctx,
    bool               strict);
```

Call after `uds_session_init()` / `uds_generated_init()`:

```c
uds_session_set_strict_programming(&session_ctx, true);
/* Now: Default → Programming → UDS_STATUS_ERR_SESSION_TRANSITION  */
/* But: Default → Extended → Programming → UDS_STATUS_OK           */
```

New field `bool strict_programming` in `uds_session_ctx_t` (zero-initialised
by `uds_session_init()` → default permissive, no struct-size regression for
callers that zero-init).

4 new unit tests in `ZTEST_SUITE(test_uds_session_strict)`.

Reported in: https://github.com/Xaloqi/EDS/issues/22, https://github.com/Xaloqi/EDS/issues/23

### Fixed — cosmetic: confusing `} else\n#endif\n{` pattern in `isotp_transmit`

Replaced with an early `return UDS_STATUS_OK` in the FD branch. No logic
change; 37/37 unit tests unchanged. Reporter mistook the construct for a
missing brace.

Reported in: https://github.com/Xaloqi/EDS/issues/18

---
## [1.7.1] — CAN FD ISO-TP support (ISO 15765-2 §9.8)

### Added — CAN FD extensions to ISO-TP layer

Four missing CAN FD paths from ISO 15765-2 §9.8, implemented in the GPL runtime.
All paths are gated behind `isotp_cfg_t.use_fd = true` — existing Classic CAN
configurations are unaffected (zero-initialised struct keeps `use_fd = false`).

**API change**: `isotp_rx_complete_cb` length parameter and `isotp_transmit` length
parameter widened from `uint16_t` to `uint32_t` for consistency with FD escape
sequence ff_dl (32-bit). Internal state fields `rx_expected_len`, `rx_received_len`,
`tx_total_len`, `tx_sent_len` also widened to `uint32_t`.

| Gap | Frame type | Spec reference | Status |
|-----|------------|----------------|--------|
| SF RX escape  | `frame->is_fd && data[0]==0x00` → read `data[1]` as SF_DL (1–62 bytes) | §9.8.2 | Fixed |
| FF RX escape  | `ff_dl==0` on FD frame → parse bytes 2–5 as 32-bit big-endian FF_DL | §9.8.3 | Fixed |
| SF TX escape  | `use_fd && length ≤ 62` → emit `data[0]=0x00, data[1]=length, is_fd=true` | §9.8.2 | Fixed |
| FF TX escape  | `use_fd && length > 4095` → emit `data[0..1]=0x10 0x00`, bytes 2–5 = 32-bit FF_DL | §9.8.3 | Fixed |

New constants in `transport/isotp.h`:
- `ISOTP_ENABLE_CAN_FD (0)` — compile-time opt-in; set to 1 to enable FD paths. Default 0: Classic CAN only, no FD code in the binary.
- `ISOTP_FD_SF_MAX_PAYLOAD_LEN (62U)` — max SF payload on CAN FD (only defined when `ISOTP_ENABLE_CAN_FD=1`)
- `ISOTP_RX_BUF_LEN` — overridable compile-time RX buffer size (default: `UDS_MAX_PAYLOAD_LEN`)

Added `bool use_fd` to both `isotp_cfg_t` and `isotp_ctx_t`. The Zephyr and FreeRTOS
platform HAL bindings still target Classic CAN only; users enabling `use_fd=true`
must supply a CAN FD-capable platform binding (tracked as a follow-up).

### Added — 8 CAN FD unit tests

New `ZTEST_SUITE(test_isotp_canfd)` in `tests/unit_runnable/test_isotp.c`:

| Test | What it covers |
|------|---------------|
| `test_fd_sf_rx_10_bytes` | FD SF 10-byte payload RX |
| `test_fd_sf_rx_62_bytes` | FD SF max 62-byte payload RX |
| `test_fd_sf_rx_zero_dl` | SF_DL=0 → `UDS_STATUS_ERR_TP_FRAME_INVALID` |
| `test_fd_sf_tx_10_bytes` | FD SF TX: byte 0=0x00, byte 1=10, `is_fd=true` |
| `test_fd_ff_escape_rx_fits` | FF escape RX (ff_dl=100): RX_WAIT_CF + FC CTS sent |
| `test_fd_ff_escape_rx_overflow` | FF escape RX (ff_dl=5000 > buffer) → FC OVFLW |
| `test_fd_ff_escape_classic_can_rejected` | FF_DL=0 on Classic CAN → FRAME_INVALID |
| `test_fd_ff_escape_tx` | FF escape TX (ff_dl=5000): bytes 0–5 encoding verified |

### Changed — example callbacks

All 7 example `on_isotp_rx_complete` callbacks updated:
`uint16_t length` → `uint32_t length`; overflow guard updated from `(uint16_t)` to
`(uint32_t)UDS_MAX_PAYLOAD_LEN`. Affects `basic_ecu`, `bms_ecu`, `sensor_ecu`,
`sensor_ecu_freertos`, `ardep_ecu`, `robot_joint_controller_ecu`, `safeboot_ecu`.

Reported in: https://github.com/Xaloqi/EDS/issues/14

---
## [1.7.0] — Robustness Campaign + SOVD Bridge

### Fixed — Protocol compliance: suppress-response bit (ISO 14229-1 §7.5.3)

Four UDS services in the generated inline simulator were returning positive
responses instead of `None` when the suppress-response bit (sub-function byte
bit 7 = `0x80`) was set by the tester.

| SID  | Service            | Bug description                                              |
|------|--------------------|--------------------------------------------------------------|
| 0x11 | ECUReset           | Returned `b'\x51\xNN'` instead of `None`                    |
| 0x28 | CommunicationControl | Returned `b'\x68\xNN'` instead of `None`                  |
| 0x31 | RoutineControl     | Also masked sub_fn incorrectly (`pdu[1]` not `pdu[1] & 0x7F`), causing NRC 0x12 on valid suppress-response requests |
| 0x85 | ControlDTCSetting  | Returned `b'\xC5\xNN'` instead of `None`                    |

Fix applied to all 11 ECU `generated/tests/conftest.py` files and to the
Jinja2 template (`tools/templates/conftest.py.j2`) that generates them:

- `examples/basic_ecu/generated/tests/conftest.py`
- `examples/basic_ecu_doip/generated/tests/conftest.py`
- `examples/basic_ecu_doip_freertos/generated/tests/conftest.py`
- `examples/basic_ecu_freertos/generated/tests/conftest.py`
- `examples/bms_ecu/generated/tests/conftest.py`
- `examples/motor_controller_ecu/generated/tests/conftest.py`
- `examples/ardep_ecu/generated/tests/conftest.py`
- `examples/robot_joint_controller_ecu/generated/tests/conftest.py`
- `examples/safeboot_ecu/generated/tests/conftest.py`
- `examples/sensor_ecu/generated/tests/conftest.py`
- `examples/sensor_ecu_freertos/generated/tests/conftest.py`

### Added — 439-test robustness campaign (Phases A–L)

Complete protocol conformance and simulator fidelity campaign across 12 phases.
All phases run in `--can-interface=simulator` mode — no hardware required.
Total: **439 tests** in `examples/basic_ecu/generated/tests/`.

| Phase | File                              | Tests | What it validates |
|-------|-----------------------------------|-------|-------------------|
| A     | `test_robustness_A_codegen.py`    | 22    | Generated file presence, C marker correctness, test file presence, GCC syntax |
| B     | `test_robustness_B_protocol.py`   | 42    | Session transitions, TesterPresent, ECUReset, all 14 service NRCs |
| C     | `test_robustness_C_security.py`   | 21    | CMAC SecurityAccess unlock/lockout, replay, all session levels |
| D     | `test_robustness_D_customer_journey.py` | 30 | Full customer workflow (fresh YAML → codegen → pytest), all 11 ECU examples |
| E     | `test_robustness_E_data_integrity.py` | 35 | DID read/write data integrity, DTC lifecycle, session isolation |
| F     | `test_robustness_F_codegen_limits.py` | 54 | Max DID/DTC/routine counts, GCC syntax gate for all 11 ECU C files |
| G     | `test_robustness_G_resilience.py` | 47    | Malformed PDU handling, CMAC end-to-end, suppress-response bit (all 6 services), YAML ↔ simulator metadata consistency |
| H     | `test_robustness_H_protocol_precision.py` | 41 | DSC timing byte precision (P2=25ms=0x0019, P2\*=5000ms=0x1388), multi-DID RDBI batching, DTC record format (3-byte code + 1-byte status), routine lifecycle |
| I     | `test_robustness_I_nrc_wdbi_sa.py` | 34   | NRC format/SID echo for every service, WDBI check ordering (session→security→length), SecurityAccess level isolation (lockout, mismatch, independent state) |
| J     | `test_robustness_J_sovd_cda.py`    | 43   | SOVD CDA semantic fidelity: top-level structure, DID/DTC/routine counts and field values, hex normalisation, semantic session names, DoIP fields present/absent, idempotency |
| K     | `test_robustness_K_error_quality.py` | 35 | Codegen error message quality: every bad YAML exits non-zero with an actionable keyword (field name, hex value, or standard reference) in stderr |
| L     | `test_robustness_L_codegen_output_fidelity.py` | 35 | Codegen output fidelity: generated C files contain correct DID decimal IDs, data lengths, access flags, timing constants, DTC severity bytes, and routine support flags |

**Phase G** (`test_robustness_G_resilience.py`, 47 tests):
- `TestMalformedPDUResilience` (13): empty PDU → NRC 0x13, unknown SID → NRC 0x11,
  truncated PDU per service, 256-SID fuzz
- `TestEndToEndCMACFlow` (11): requestSeed → CMAC → sendKey round-trip, wrong key →
  NRC 0x35, 3-failure lockout → NRC 0x36, seed randomness, L1+L2 independent
- `TestSuppressResponseBit` (11): all 6 services with sub-function, verifies `None`
  response when bit 7 set (exposed the 4 compliance bugs above)
- `TestYAMLSimulatorConsistency` (12): parse `diagnostics_config.yaml` and verify
  `_dids_meta` and `_routines_meta` in the inline simulator match exactly

**Phase H** (`test_robustness_H_protocol_precision.py`, 41 tests):
- `TestDSCTimingPrecision` (8): P2/P2\* big-endian byte values exact across all
  3 session types; re-entry timing unchanged
- `TestMultiDIDReadByIdentifier` (11): 2/3/4/5 DID batch read, echo order, NRC on
  unknown DID in batch, WDBI→RDBI round-trip, session-gated DID
- `TestReadDTCPrecision` (11): sub 0x01 layout (6 bytes, count field), sub 0x02
  4-byte record format, DTC code byte order (`0xC00100` → `[0xC0, 0x01, 0x00]`)
- `TestRoutineControlLifecycle` (11): stop-before-start → NRC 0x22,
  results-before-start → NRC 0x22, security-gated, programming session, re-start

**Phase I** (`test_robustness_I_nrc_wdbi_sa.py`, 34 tests):
- `TestNRCFormatAndSIDEcho` (13): for every service, NRC response is exactly 3 bytes
  `[0x7F, requestSID, NRC_code]` — verifies SID echo and length (ISO 14229-1 §7.5.2)
- `TestWDBICompleteness` (12): correct WDBI succeeds, 1-byte short/long/zero → NRC 0x13,
  read-only DID → NRC 0x31, unknown DID → NRC 0x31; check ordering: session gate fires
  before security gate, security gate fires before length gate
- `TestSecurityAccessProtocolEdges` (9): level mismatch (seed L1, key L2) → NRC 0x24,
  pending seed cleared after successful unlock, 1 wrong key then correct unlocks,
  L1 lockout does not block L2, L1 unlock does not grant L2

**Phase J** (`test_robustness_J_sovd_cda.py`, 43 tests):
- `TestCDAStructure` (8): required top-level keys, sovdVersion=1.0.0, generatedBy mentions
  Xaloqi, generatedAt non-empty, ecuIdentification name/version match YAML, JSON round-trip
- `TestCDADIDFidelity` (10): count, hex-normalised IDs, names, dataLengthBytes, access
  lists, semantic minSession strings, None writeSecurityLevel for read-only DIDs,
  int writeSecurityLevel for read-write DIDs, readSecurityLevel match
- `TestCDADTCFidelity` (6): count, hex codes, descriptions, valid severity set, severity match
- `TestCDARoutineFidelity` (7): count, hex IDs, names, semantic sessions, securityLevel,
  supportedSubFunctions match YAML support list
- `TestCDATransportAndDoIP` (12): CAN → ISO-TP, no logicalAddress/port for CAN; DoIP →
  DoIP protocol, port=13400, logicalAddress=0xE400, sourceAddress present; transport=both →
  DoIP; 14 diagnosticServices each with sid+name; two-call idempotency

**Phase K** (`test_robustness_K_error_quality.py`, 35 tests):
- `TestSanity` (2): valid YAML exits 0 and prints dry-run-complete message
- `TestMetadataErrors` (5): missing metadata section, missing ecu_name, missing version,
  schema_version mismatch (99 → rejected with "schema_version" keyword),
  schema_version wrong type ("one" → rejected with "schema_version" keyword)
- `TestDIDErrors` (13): id wrong hex length, invalid hex chars, no 0x prefix, duplicate DID
  (asserts "already declared"), reserved id 0x0000, missing name, missing access,
  invalid access value ("execute"), unknown min_session ("factory"),
  read_security_level out of range (300 → asserts "255"), data_length zero,
  data_length over max (9999 → asserts "4095"), write DID missing data_length
  (asserts "REQ-SAFE-006")
- `TestDTCErrors` (5): code wrong hex length, missing 0x prefix, invalid chars,
  missing code field, duplicate DTC codes (asserts "already declared")
- `TestTimingErrors` (3): p2 > p2_star, p2 zero, timing wrong type ("fast")
- `TestRoutineErrors` (3): routine id wrong hex length, invalid chars, duplicate routine ids
- `TestParseErrors` (3): unclosed bracket YAML, empty file, non-mapping root (list)

Also added to `codegen.py` `validate_config()`:
- `schema_version` validation: must be an integer equal to `SUPPORTED_SCHEMA_VERSION` (1).
  Non-integer or unsupported version exits with actionable error naming the field.
- Write-DID safety gate (REQ-SAFE-006): writable DIDs without an explicit `data_length`
  are now rejected with a message referencing REQ-SAFE-006 and the 0x2E handler.

**Phase L** (`test_robustness_L_codegen_output_fidelity.py`, 35 tests):
- `TestSanity` (2): codegen exits 0, all expected files generated
- `TestGeneratedConfigH` (8): p2/p2*/s3 timing values, DID/DTC counts, ECU name,
  version, and include guard all match the controlled test YAML
- `TestDIDHandlersH` (5): handler count macro (2U), read declarations for both DIDs,
  write declaration for read-write DID, no spurious write declaration for read-only DID
- `TestDIDHandlersC` (10): 0xF190→61840U, 0xF187→61831U, data_length 17U/11U,
  DID_ACCESS_READ|WRITE flag for F187, exactly one WRITE flag in file, session constants,
  write_cb function name set for F187, write_cb = NULL exactly once (F190)
- `TestDTCUDSInit` (5): 0xC00100→12583168UL, severity 0x20U, description text,
  exactly one `dtc_database_register` call site, no 0x40U (maintenance_only absent)
- `TestRoutineHandlersC` (5): 0xFF00/0xFF01 present, ROUTINE_SUPPORT_START|RESULTS
  for FF00, UDS_SESSION_PROGRAMMING for FF01, no results stub for start-only routine

### Changed — CI

`.github/workflows/ci.yml` `robustness-tests` job updated:
- Phase count: 6 phases / 245 tests → 12 phases / 439 tests
- Added individual `pytest` steps for Phases G, H, I, J, K, L with short descriptions
- Final assertion: `439 passed`
- Phase comments updated with per-phase test counts A(22) B(42) C(21) D(30) E(35)
  F(54) G(47) H(41) I(34) J(43) K(35) L(35)

`.github/workflows/ci.yml` `integration-tests` job: added `--ignore` flags for
`test_robustness_G_resilience.py`, `test_robustness_H_protocol_precision.py`,
`test_robustness_I_nrc_wdbi_sa.py`, `test_robustness_J_sovd_cda.py`,
`test_robustness_K_error_quality.py`, `test_robustness_L_codegen_output_fidelity.py`
(they run in the dedicated `robustness-tests` job).

`test_robustness_D_customer_journey.py` `TestAllECUExamplesPytest`: added `--ignore`
for Phases G, H, I, J, K, L to prevent recursive collection when running all 11 ECU examples.

### Added — SOVD CDA codegen output

`tools/codegen.py --sovd`: new optional flag that generates a valid OpenSOVD 1.0
Capability Description and Advertisement (CDA) JSON file (`sovd_cda.json`) alongside
the standard C output. Pure-Python implementation — no Jinja2 template required.
`build_sovd_cda(cfg)` builds the CDA dict directly; `render_sovd_cda()` writes it
with `json.dumps(indent=2)`.

The CDA captures the full ECU diagnostic profile from `diagnostics_config.yaml`:

- All configured DIDs with `id`, `name`, `dataLengthBytes`, `access`, `minSession`,
  `readSecurityLevel`, `writeSecurityLevel`
- All configured DTCs with `code`, `description`, `severity`
- All configured routines with `id`, `name`, `minSession`, `securityLevel`,
  `supportedSubFunctions`
- Static list of all 14 EDS UDS services (`diagnosticServices`)
- `transportInfo.protocol`: `"DoIP"` or `"ISO-TP"` derived from `ecu.transport`
- `ecuIdentification.logicalAddress`, `ecuIdentification.sourceAddress`,
  `transportInfo.port`: present only when `transport` is `doip` or `both`

Session names use semantic strings (`"default"`, `"extended"`, `"programming"`) —
not internal C constants — so the JSON is directly readable by SOVD clients and
Eclipse SDV tooling.

### Changed — CI

`.github/workflows/ci.yml`: added `sovd-codegen` job (job 14 of 14). Imports
`build_sovd_cda` and `load_config` directly from `tools/codegen.py` — no template
checkout required. Validates CDA structure, transport protocol, DID/DTC/routine
counts, and presence/absence of `logicalAddress` for CAN vs DoIP ECUs.

### Fixed — Pre-launch audit (v1.7.0 patch)

- `tools/codegen.py`: added `__version__ = "1.7.0"` module-level constant.
  Banner previously printed "Phase 4" (internal development label); now prints
  `"Xaloqi EDS — Code Generator v1.7.0"` using the constant.
- `tools/codegen.py` `build_sovd_cda()`: `generatedBy` field corrected from
  `"Xaloqi EDS codegen v1.6.0"` to `"Xaloqi EDS codegen v1.7.0"`.
- `INSTALL.md`: template count corrected 14 → 17; expected codegen output block
  replaced with the actual `[1/5]…[5/5]` step format.
- `tools/testlab.py` (`__version__`): synced from `"1.2.0"` to `"1.4.0"` to match
  the current TestLab product version.

---

## [1.6.0] — DoIP (ISO 13400-2) transport

### Added — DoIP server for Zephyr and FreeRTOS

transport/doip/doip_server.h + doip_server.c: ISO 13400-2 ECU server.
Platform-agnostic core via eds_doip_platform_ops_t. No malloc, no recursion,
static buffers. Covers: Routing Activation (Default type), DiagnosticMessage
dispatch → uds_server_process_request(), Positive/Negative Ack, Alive Check.
Symmetric with xaloqi-tester DoipBus (TestLab v1.1.0) — byte-for-byte frame
format compatibility.

transport/doip/zephyr_lwip.h + zephyr_lwip.c: Zephyr BSD-socket binding.
Implements eds_doip_platform_ops_t via zsock_*. K_THREAD_DEFINE creates the
DoIP server thread automatically at startup.

transport/doip/freertos_lwip.h + freertos_lwip.c: FreeRTOS + LwIP binding.
Implements eds_doip_platform_ops_t via lwip_socket API. xTaskCreate() creates
the DoIP server task; configurable stack size and priority.

platform/zephyr/platform_doip.h + platform_doip.c: Zephyr DoIP registration
shim. Exposes eds_doip_platform_start() for application main.c.

platform/freertos/platform_doip.h + platform_doip.c: FreeRTOS DoIP registration
shim. Exposes eds_doip_platform_start_freertos() for application main.c.

examples/basic_ecu_doip/: New Zephyr example ECU — same 5 DIDs / 2 DTCs /
3 routines as basic_ecu, served over DoIP on native_sim (loopback networking).
EDS_DOIP_ONLY_BUILD=1 disables ISO-TP init; uds_generated_init(NULL, 0, 0).

examples/basic_ecu_doip_freertos/: New FreeRTOS + LwIP example ECU — same
schema, FreeRTOS platform, LwIP TCP. LwIP stub headers for CI compile testing.

tests/unit_runnable/test_doip_server.c: 24 host-side Unity tests covering
header encode/decode, routing activation, alive check, diagnostic message
dispatch, NACK generation, boundary conditions, and NULL-pointer guards.

tests/test_doip_integration.py: 10 pytest end-to-end integration tests
(skipped automatically when xaloqi-tester not installed).

### Changed — CI

.github/workflows/ci.yml: Added doip-integration job (native_sim build +
unit tests smoke check + pytest integration tests with graceful skip on
missing xaloqi-tester). Exit code 5 (all tests skipped) treated as success.

misra_analysis.py: DEV-GOTO-01 (Rule 15.1 — goto in connection teardown),
DEV-FD-01 (Rule 11.6 — void*/int fd cast, same pattern Zephyr and FreeRTOS
bindings), plus extensions to DEV-MULT-01, DEV-PREC-01, DEV-CAST-02,
DEV-MCRO-01, DEV-ACCS-01, DEV-LOOP-01, DEV-GEN-01, DEV-TYPE-03 for all
new transport/doip and platform/freertos/platform_doip files.

### Schema change — diagnostics_config.yaml (additive, backward-compatible)

Optional ecu.transport field: "can" (default), "doip", or "both".
Optional ecu.doip block: logical_address, source_address, port.
Existing configs without these fields continue to build unchanged.

---

##  [1.5.0] — TestLab integration + testgen refactor
### Added — testlab_config.yaml standalone mode (TestLab)

xaloqi/tester/_config.py: full input validation with precise error messages
for every bad input — CAN ID out of range, unknown session names, negative
data_length, invalid DTC severity, missing required DID/DTC fields.
Error messages include field name and array index (dids[1]: 'min_session' must be one of ...).
testlab_config.yaml: documented template file at the TestLab repo root.
Copy-paste starting point for customers not using Xaloqi EDS.
campaigns/standalone_validation.yaml: four ready-to-run campaign jobs
(basic_validation, eol_production_check, security_audit,
regression_check) for non-EDS users.
load_testlab_config() and load_eds_config() now cross-reject each other
with a clear error when the wrong format is passed.
runner.py --config help text updated to mention both EDS YAML and
standalone testlab_config.yaml formats.

### Changed — testgen.py conftest refactor (EDS-toolchain + EDS)

tools/templates/conftest.py.j2 reduced from 870 lines to 456 lines.
Inline ISO-TP framing (300 lines), AES-128 S-box + CMAC (200 lines), and
ECU simulator (300+ lines) replaced by xaloqi-tester imports:
UdsTester.raw_request(), aes_cmac(), derive_key().
Public API of the generated conftest.py is unchanged — all test files
(test_did_*.py, test_services.py, test_routine_*.py) work without
modification after the template update.
All generated conftest.py files in all 7 specialist examples regenerated.
requirements_testgen.txt now lists xaloqi-tester>=1.0.0.
Bug fixes in ISO-TP or AES-CMAC applied to xaloqi-tester now propagate
automatically to all generated test suites — no more diverging inline copies.

### Added — PCAN/Kvaser hardware backends (TestLab)

xaloqi/tester/transport/hardware.py: HardwareBus, PcanBus, KvaserBus.
Wraps any python-can >= 4.0 adapter by bustype string. PCAN and Kvaser are
named convenience subclasses with driver-specific error messages and
troubleshooting hints.
Supports all python-can hardware: PCAN USB/PCIe, Kvaser USB, IXXAT,
Vector CANalyzer/CANoe, SLCAN, and bustype="auto" for auto-detection.
PcanBus("PCAN_USBBUS1") and KvaserBus(0) pass directly as the
interface argument to UdsTester.
tests/test_hardware.py: 25 unit tests (fully mocked, plus
@pytest.mark.hardware markers for tests requiring physical adapters).
tests/conftest.py: --hardware CLI flag registers
@pytest.mark.hardware skip logic. Hardware tests are excluded from CI
automatically and re-enabled with pytest --hardware.

### Added — production audit fixes (TestLab)

License enforcement in UdsTester.__aenter__() now actually executes —
previously a pass stub. Raises LicenseError with purchase URL when no
key is found.
Bare assert in seven service methods replaced with TransportError —
AssertionError is suppressed by Python's -O flag and gives no diagnostic.
SocketCanBus.__aenter__() / __aexit__() added — sim.py used
async with SocketCanBus(...) which would crash without these.
Dead isotp_recv() / isotp_send() functions removed from docker/ecu_sim/sim.py.
SPDX headers added to all 16 source files in xaloqi/, tools/, docker/.
xaloqi/__version__ = "1.0.0" added.
LICENSE_COMMERCIAL.txt created.
[project.urls] added to pyproject.toml.

## [1.4.0] — Job Engine + CI expansion

### Added — Job Engine (IDEA-032)

- `tools/jobrunner.py` — executes YAML-defined multi-step UDS workflow jobs
  against a real ECU (SocketCAN) or simulated ECU (harness binary). The same
  job definition runs identically in CLI, pytest, CI pipeline, and AI agent.
- `jobs:` top-level block in `diagnostics_config.yaml` — optional, backward
  compatible. Existing configs without `jobs:` continue to work unchanged.
- 15 action types: `session`, `security_access`, `read_did`, `write_did`,
  `read_dtc`, `clear_dtc`, `routine`, `foreach_did`, `assert`, `ecu_reset`,
  `tester_present`, `delay`, `request_download`, `transfer_data`,
  `request_transfer_exit`.
- Variable interpolation: `save_as` stores response bytes; `${name}` references
  them in subsequent steps. Used for firmware size in flash workflows.
- JSON output (`--json`): structured result file with `schema_version: 1`.
  Stable interface contract for CI reporting and TestLab AI (roadmap).
- `tools/job_library/` — 5 pre-built job templates: `flash_and_verify`,
  `eol_production_check`, `field_diagnostic_read`, `calibration_sequence`,
  `security_lockout_reset`.
- `tools/config_parser.py`: `jobs:` block is now validated structurally
  (unknown actions, missing `steps`, invalid `on_failure` values).

### Added — sensor_ecu example

- `examples/sensor_ecu/generated/` — all C/H generated files now committed
  (`uds_init.c`, `did_handlers.c`, `did_safety_wrappers.c`, `safety_config.h`,
  `generated_config.h` + full test suite).
- `examples/sensor_ecu/diagnostics_config.yaml` — 5 working Job Engine jobs:
  `field_diagnostic_read`, `sensor_health_check`, `calibration_reset`,
  `calibration_write`, `dtc_clear_and_verify`.

### Added — CI

- EDS-toolchain CI expanded from 7 to 16 jobs.
- 7 new example validation jobs (one per specialist example): YAML schema,
  generated file presence, safety markers (`uds_safety_self_test`,
  `keys_are_placeholder`), DID count verification, test file presence.
- `gui-build` job: TypeScript typecheck + Vite production build.
- `validate-harness` job: Python tester import validation + `derive_key` smoke test.
- `validate-jobrunner` job: dry-run all example configs + job library schema
  validation + 43 unit tests (mock UdsTester, no hardware required).

### Added — scripts

- `scripts/verify_did_counts.py` added to EDS-toolchain Developer ZIP.
  Previously only in the public EDS repo.

### Fixed — GUI

- `gui/package-lock.json` regenerated with full sha512 integrity hashes.
  Previous lockfile was missing hashes for 48/49 packages, causing `npm ci`
  to install incomplete packages and fail at runtime.
- `gui/package.json`: added `react-refresh@0.14.0` as explicit devDependency
  (peer dep of `@vitejs/plugin-react@4.2.1`).

### Documentation

- `INSTALL.md` — Job Engine section with full CLI reference and job template table.
- `docs/INTEGRATION_GUIDE.md` — Section 6: Job Engine full reference (actions
  table, variable interpolation, CLI examples, JSON schema).
- `docs/AI_CONTEXT.md` — `jobs:` YAML schema with all 15 actions documented;
  `jobrunner.py` and `job_library/` in repo structure.
- All docs bumped to v1.4.0.

---

## [1.3.0] — Platform housekeeping + FreeRTOS API

### Fixed — Platform structure

- Removed 15 duplicate files from `platform/` root — all were byte-for-byte
  identical to their `platform/zephyr/` counterparts. CMakeLists already
  compiled from `platform/zephyr/`; root copies were dead code.
- Moved `transport/zephyr_can.c/.h` to `platform/zephyr/` — Zephyr-specific
  CAN driver belongs in the platform layer, not the RTOS-agnostic transport layer.
- Removed stale `transport/zephyr_port.c/.h` — canonical copy already in
  `platform/zephyr/` with updated include guard and missing declaration added.
- Updated `CMakeLists.txt` to compile `zephyr_can.c` from `platform/zephyr/`.
- Removed `scripts/sync_shadow_copies.sh` — stub with no function.

### Fixed — Stack safety

- `core/uds_types.h`: added `_Static_assert` that fires at compile time if
  `sizeof(uds_msg_buf_t)` exceeds `EDS_MSG_BUF_MAX_STACK_BYTES` (default 256).
  Catches accidental stack allocation of the 4097-byte message buffer on embedded
  targets. Suppress with `-DEDS_MSG_BUF_MAX_STACK_BYTES=8192` for host/sim builds.
- `core/uds_safety.c`: replaced two stack-allocated `uds_msg_buf_t` instances in
  `uds_safety_self_test()` with module-level statics (`s_self_test_req_a/b`).
  Previous code allocated ~8194 bytes on the stack — more than typical task stacks.

### Fixed — Security

- `core/uds_security.c`: `[SEC-ENTROPY-01]` — `uds_security_request_seed()` now
  rejects all-zero seeds with `UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE`. Prevents
  silent security failure from uninitialised TRNG peripherals.
- `platform/freertos/freertos_platform_api.c`: removed `vTaskDelay(2)` before
  NVIC reset. Replaced with TX CONFIRMATION CONTRACT comment specifying the
  correct caller sequence: `isotp_transmit()` → poll until TX IDLE →
  `eds_platform_nvm_flush()` → `eds_platform_ecu_reset()`.

### Added — FreeRTOS integration API

- `platform/freertos/freertos_platform_api.c` + `platform/platform_api.h`:
  new `eds_freertos_start()` API encapsulates the UDS poll task creation, ISO-TP
  RX callback, and static task storage. FreeRTOS integrators now call four
  functions instead of copying 80 lines of boilerplate.
- `examples/basic_ecu_freertos/src/main.c`: simplified using `eds_freertos_start()`.
  This example is now the canonical FreeRTOS integration reference.

### Added — Tests

- `tests/unit_runnable/test_isotp_concurrent.c`: 6 new test cases covering
  ISO-TP concurrent request scenarios — SF interrupting multi-frame, new FF
  restarting reassembly, CF-without-FF, wrong SN recovery, N_Cr timeout recovery.

### Added — Tooling and validation

- `tools/config_parser.py`: `schema_version` field validation — missing field
  emits a deprecation warning; version mismatch is a hard error.
- `data_length` now required for all write-capable DIDs (REQ-SAFE-006 enforcement).

### Added — Documentation

- `docs/SECURITY_NOTICE.md`: FreeRTOS seed entropy requirements with STM32/NXP
  TRNG code examples and ISO 26262 / UNECE WP.29 references.
- `docs/INTEGRATION_GUIDE.md`: section 4 rewritten with full Five-Step FreeRTOS
  integration using `eds_freertos_start()`; ECU reset TX confirmation contract.
- `platform/platform_api.h`: mutex interface split documented with rationale.

---

## [1.3.0] — SafeBoot + Sensor + Robotics Examples

**Status: All 16 CI jobs green. Three new examples. SafeBoot codegen automation complete.**

### Added — SafeBoot (MCUboot DFU over UDS)

- `safeboot:` YAML block in `diagnostics_config.yaml`. Set `enabled: true` to
  generate `zephyr_flash_ops_init()` automatically into `uds_init.c`. No manual
  flash ops registration required in application code.

- `tools/codegen.py` — `build_uds_init_context()` reads `safeboot.enabled`,
  `safeboot.platform`, `safeboot.max_block_length` and passes them to the template.

- `tools/templates/uds_init.c.j2` — Step 5.7 now generates conditionally:
  - `safeboot.enabled: true` → `#include "zephyr_flash_ops.h"` + `zephyr_flash_ops_init()`
    call with full REQ-FLASH-001/002/003 safety comments.
  - `safeboot.enabled: false` (default) → documentation comment explaining how to
    enable, no code generated. Existing behaviour fully preserved.

- `tools/config_parser.py` — `safeboot:` block added to `CONFIG_SCHEMA`.

- `examples/safeboot_ecu/` — complete MCUboot DFU example targeting
  STM32 Nucleo-H743ZI2. Includes 7-step DFU sequence documentation and
  `dfu_flash.py` Python script using `udsoncan`.

- `.github/workflows/ci.yml` — `safeboot-example` job (job 15 of 16):
  verifies `zephyr_flash_ops_init()` is generated when enabled, and that
  `basic_ecu` (disabled) does not regress.

### Added — SensorECU example (IDEA-036/037)

- `examples/sensor_ecu/` — zone controller demonstrating the complete
  sensor → DID → DTC pattern using the Zephyr sensor API.
  Temperature (DID 0xD001) and supply voltage (DID 0xD002) read via
  `sensor_sample_fetch()` / `sensor_channel_get()` every 100 ms.
  DTCs 0xD00101/102 (over/under temperature) and 0xD00201/202
  (over/under voltage) set and cleared automatically by `sensor_monitor.c`.
  Writable calibration thresholds via DID 0xD010/0xD011.

- `examples/sensor_ecu/src/sensor_monitor.c` — dedicated 100 ms monitoring
  thread. Calls `dtc_database_set_status()` on threshold violations.
  DID handlers read from a mutex-protected `sensor_state_t` cache —
  never block the 1 ms UDS poll loop.

- `.github/workflows/ci.yml` — `sensor-example` job (job 13 of 16).

### Added — Robot Joint Controller example (IDEA-021)

- `examples/robot_joint_controller_ecu/` — joint controller ECU for a
  single-axis servo robot. 10 DIDs (position, velocity, torque, temperature,
  status, calibration limits), 5 DTCs (over-temperature, over-current,
  encoder loss, soft limit exceedances), 3 routines (home axis, apply
  calibration, clear fault history).

- README written for robotics engineers: explains why UDS is used in robotics
  (standard toolchain, ISO-TP handles multi-byte payloads, DTC persistence),
  includes Python `udsoncan` snippet for reading live joint state.

- `.github/workflows/ci.yml` — `robotics-example` job (job 14 of 16).

### Changed — CI

- CI now has 16 jobs (was 13 at v1.2.0). New jobs: `sensor-example`,
  `robotics-example`, `safeboot-example`.

- CI header updated with full 16-job index.

---

## [1.2.0] — FreeRTOS Platform Support

**Status: All 13 CI jobs green. FreeRTOS HAL complete. Zephyr builds unaffected.**

### Added — FreeRTOS platform HAL

- `platform/platform_api.h` — platform-neutral interface implemented by both HALs.
  Declares `eds_platform_ecu_reset()`, `eds_platform_nvm_flush()`,
  `eds_platform_uptime_ms()`, `eds_platform_init()`, `eds_platform_can_input()`,
  and the `eds_can_frame_t` / `eds_nvm_ops_t` / `eds_platform_cfg_t` types.

- `platform/freertos/freertos_platform_api.c` — implements `platform_api.h` for
  FreeRTOS. Customer provides a `can_send` callback and optional NVM ops via
  `eds_platform_init()`. Built-in RAM NVM stub used when no flash driver is provided
  (development / CI). ECU reset via direct SCB AIRCR write (all Cortex-M variants).
  Optional customer reset hook via `eds_platform_set_reset_cb()`.

- `platform/freertos/freertos_can.c/.h` — implements `can_transport_ops_t` over a
  static `xQueueCreateStatic` RX queue (8 frames, no heap). `freertos_can_input()`
  is ISR-safe via `xQueueSendFromISR`. `freertos_can_set_bus_off()` called from
  customer CAN error interrupt. Full `eds_can_frame_t` ↔ `uds_can_frame_t` conversion.

- `platform/freertos/freertos_nvm.c` — implements `nvm_store_*` API routing through
  customer-provided `eds_nvm_ops_t` callbacks. Schema version check and migration on
  init. Guarded by `EDS_PLATFORM_FREERTOS` so it never compiles into Zephyr builds.

- `examples/basic_ecu_freertos/` — FreeRTOS example using stub CAN loopback for CI.
  Same `diagnostics_config.yaml` as `examples/basic_ecu/` — proves same YAML generates
  working firmware on both platforms. Includes `boards/qemu_cortex_m4/FreeRTOSConfig.h`
  and linker script targeting QEMU `mps2-an386`.

- `cmake/toolchain/arm-none-eabi.cmake` — CMake toolchain file for bare-metal
  ARM cross-compilation. Sets `CMAKE_SYSTEM_NAME=Generic`,
  `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY` (skips linker probe that fails without
  `crt0.o`), and `CMAKE_SYSROOT=/usr/lib/arm-none-eabi` (resolves newlib headers from
  Ubuntu `libnewlib-arm-none-eabi` package). Auto-detects `arm-none-eabi-gcc` or
  `arm-zephyr-eabi-gcc`.

- `.github/workflows/ci.yml` — `freertos-qemu` job (job 13 of 13). Clones
  `FreeRTOS-Kernel`, runs codegen, cross-compiles with `arm-none-eabi-gcc` for QEMU
  Cortex-M4, verifies ELF exists and text segment is non-zero, uploads artifact.

### Changed — platform directory restructured

- All Zephyr-specific platform files moved from `platform/` root into `platform/zephyr/`.
  The `platform/` root now contains only `platform_api.h` (shared interface) and
  `uds_flash_ops.c/.h` (platform-independent flash ops registration).

- `platform/zephyr/zephyr_platform_api.c` added — implements `eds_platform_*` for
  Zephyr using `k_uptime_get_32()`, `sys_reboot()`, and the existing NVM flush logic
  from `zephyr_port.c`.

- Root `CMakeLists.txt` and all example `CMakeLists.txt` files updated: `EDS_PLATFORM`
  CMake variable selects `zephyr` (default, unchanged behaviour) or `freertos`.

- `tests/mocks/zephyr_port_mock.c` — updated includes and added `eds_platform_*` stubs.

- `build_tests.sh` — `platform/zephyr/` added to include path; `nvm_store_mock.c`
  path updated from `platform/nvm_store_mock.c` to `platform/zephyr/nvm_store_mock.c`.

### Changed — CI

- `tests/CMakeLists.txt` — `DIAG_PLATFORM_ZEPHYR` variable added; `platform/zephyr/`
  added to include dirs; all source paths updated to `platform/zephyr/` prefix.

- `core/uds_services/service_0x11.c` — removed `#include "zephyr_port.h"` (service
  only sets `ctx->pending_reset_type`; never called platform functions directly).

---

## [1.1.0] — Commercial Readiness

**Status: Pre-release hardening. All CI jobs green. Codebase is feature-complete for v1.1.x
evaluation builds. Remaining open items are licensing, hardware validation, and community
publishing — none block technical evaluation.**

### Added — CI safety assertions (HIGH-1 / CRIT-2)
- `.github/workflows/ci.yml` — new step **"Assert ASIL_B_REQUIRE_WRITE_SECURITY is True
  (HIGH-1)"** in the `unit-tests` job. A flip of the flag to `False` in `tools/codegen.py`
  now fails CI immediately rather than silently downgrading write-security enforcement from
  a fatal codegen error to an advisory warning.
  Traceability: HIGH-1 / ISO 26262-6 / ASIL-B write-access policy.
- `.github/workflows/ci.yml` — new steps **"Assert safety invariants in ARDEP / BMS / MC
  generated output (CRIT-2 / HIGH-1)"** in the `ardep-example`, `bms-example`, and
  `mc-example` jobs. Each independently verifies: `uds_safety_self_test()` present,
  abort-on-failure guard (`return self_test_rc`) present, and `ASIL_B_REQUIRE_WRITE_SECURITY`
  intact — applied to that job's freshly-regenerated `uds_init.c`.
  Previously the self-test assertion only covered `basic_ecu` in the `unit-tests` job.
  Traceability: REQ-SAFE-SELFTEST-01 / ISO 26262-6 §9.4.3.

### Added — Repo & documentation
- `SECURITY.md` (repo root) — canonical security policy; GitHub surfaces the
  "Report a vulnerability" button when this file is present at root.
- `CONTRIBUTING.md` (repo root) — canonical contribution guide; explains dual-license
  CLA requirement, coding conventions, PR checklist, and what is / is not accepted.
- `docs/SECURITY.md` and `docs/CONTRIBUTING.md` replaced with thin redirect stubs
  pointing to the canonical root files.
- `README.md` rewritten as a product page: problem-first structure, 60-second walkthrough
  with realistic YAML, CI-verified safety properties called out explicitly, placeholder
  key warning surfaced, links to `docs/` instead of duplicating content.

### Removed
- `tests/legacy_unit/` deleted. The 15 modules it contained are a strict subset of the
  canonical 35-module set in `tests/unit_runnable/`. Both `build_tests.sh` and
  `tests/CMakeLists.txt` have always referenced `unit_runnable/` — the archived copy
  was purely confusing. (M2)
- Binary artifacts (M1) and shadow source copies in `project/src/` (M3) were already
  removed in a prior phase; confirmed absent.

### Changed
- `tests/CMakeLists.txt` — M2 annotation updated to reflect `legacy_unit/` removed
  (not merely archived).
- `docs/PHASE1_SECURITY_CHANGES.md` — M2 row updated to final resolution status.
- `.github/workflows/ci.yml` — `[HIGH-1-CI]` and `[CRIT-2-CI]` entries added to the
  FIXES header block.

### Notes
- All licensing decisions (D1–D10) resolved. GPL v2 runtime, commercial toolchain.

## [1.1.0] — Layer 4 + Layer 5 Complete

**Status: All v1.0.0 tests still passing. New CAPL generation and VS Code extension added.**

### Added — CANoe CAPL Test Generation (`--capl` flag in `testgen.py`)

- `tools/testgen.py` v1.1.0: new `--capl` and `--capl-only` CLI flags
- `tools/templates/ecu_diagnostics_test_suite.can.j2` — master CANoe test module template:
  - Full ISO-TP transport layer in CAPL (SF / FF / CF / FC, Flow Control, 0x78
    response-pending loop)
  - `on message kTxCanId` handler for frame reassembly (SF/FF/CF/FC)
  - Shared UDS helpers: `Uds_EnterSession`, `Uds_UnlockLevel`, `Uds_ReadDid`,
    `Uds_WriteDid`, `Uds_ClearDtcs`, `Uds_EcuReset`
  - Assert helpers: `AssertPositiveResponse`, `AssertNegativeResponse`,
    `AssertResponseLength`
  - Security key arrays (`kSecKeyLevelN[16]`) with AES-CMAC and XOR-stub derivation
  - Core service testcases: `TC_Services_DefaultSession`, `TC_Services_ExtendedSession`,
    `TC_Services_ProgrammingSession`, `TC_Services_TesterPresent`,
    `TC_Services_TesterPresentSuppressed`, `TC_Services_EcuReset_Hard`,
    `TC_Services_EcuReset_Soft`, `TC_Services_SessionTimeout`, SecurityAccess testcases
  - DID smoke testcases (`TC_DID_Read_Smoke_XXXX`) per configured DID
  - `testgroup TG_CoreServices`, `testgroup TG_DID_SmokeTests`,
    `maintest <ECU>_DiagnosticsSuite`
- `tools/templates/test_did_XXXX.can.j2` — per-DID exhaustive test module:
  - DID constants, `SetupRead_XXXX()` / `SetupWrite_XXXX()` helpers
  - Conditionally generated testcases per YAML access policy
  - `testgroup TG_DID_XXXX` runner
- `tools/templates/test_dtcs.can.j2` — DTC service test template:
  - DTC code constants, helpers, RDTCI sub-function testcases, `testgroup TG_DTCTests`
- `testgen.py`: `_build_security_levels()` adds `default_key_bytes` for CAPL key arrays
- `testgen.py`: `code_hi`, `code_mid`, `code_lo` pre-computed in DTC context
- `testgen.py`: `_capl_readme()` generates `README_CANOE.md` with import instructions
- Scale: `basic_ecu` (5 DIDs, 2 DTCs) → 8 `.can` files, 47 `testcase` functions

### Added — VS Code Extension (`ide/vscode-extension/`)

- `src/extension.ts` — activation on `onLanguage:yaml`, command registrations,
  status bar item, auto-save hook
- `src/validator.ts` — inline YAML diagnostics: DID/DTC format, duplicates,
  `data_length > 64`, enum values, write-security ASIL-B warning
- `src/hoverDocs.ts` — documentation for every YAML key with ISO 14229 context
- `src/hoverProvider.ts` — key-path resolver + `HoverProvider` implementation
- `src/codegenRunner.ts` — terminal-based codegen execution with QuickPick flag picker
- `schemas/diagnostics_config.schema.json` — full JSON Schema for
  `diagnostics_config.yaml`
- Commands: `EDS: Run Codegen`, `EDS: Run Codegen (with options)`,
  `EDS: Validate diagnostics_config.yaml`, `EDS: Open Documentation`

### Changed

- `tools/testgen.py` version bumped to 1.1.0; `--capl`/`--capl-only` flags added;
  fully backward-compatible (no `--capl` = identical v1.0.0 behaviour)
- `CLAUDE.md`: absolute rule #8 added (never use `>>` inside Jinja2 `{{ }}`);
  `ide/` directory added to repo tree; CAPL build commands added

---

## [1.0.0] — Phase 9 Complete

**Status: All tests passing. 35/35 unit tests. 68/68 harness tests.**

### Added
- 14 UDS service handlers: 0x10, 0x11, 0x14, 0x19, 0x22, 0x27, 0x28, 0x2E, 0x31,
  0x34, 0x36, 0x37, 0x3D, 0x3E
- ISO-TP transport: SF, FF, CF, FC with full N_As/N_Bs/N_Cs/N_Cr timing parameters
- ASIL-B 5-step safety wrapper chain enforced by code generator on every DID access
- AES-128-CMAC SecurityAccess (0x27) — production security algorithm
- YAML-driven code generator (`tools/codegen.py`) with 8 Jinja2 templates
- Auto-generated pytest test suites (`tools/testgen.py`)
- 4 reference ECU examples: basic_ecu, bms_ecu, motor_controller, ardep
- React/TypeScript live dashboard GUI + bridge.py WebSocket bridge
- 12-job GitHub Actions CI pipeline
- STM32 Nucleo-H743ZI2 board overlay and build configuration
- DTC NVM mirror with `dtc_mirror_init()` / `dtc_mirror_load()` in init sequence
- 35 Unity unit test modules (all passing)
- 68 harness integration tests (all passing)
- `uds_safety_self_test()` callable at boot for runtime safety self-check
- Violation counters and `last_violation_code` for field diagnostics
- Requirement traceability tags REQ-SAFE-001 through REQ-SAFE-007

### Fixed (Phase 9 repairs)
- P9-H1: `basic_ecu/CMakeLists.txt` — added 6 missing DFU sources
- P9-M1: Root `CMakeLists.txt` — `CONFIG_BOARD_NATIVE_SIM` conditional for
  `nvm_store` + `zephyr_flash_ops`
- P9-M2: `ci.yml` — `npm install` → `npm ci`; `cache-dependency-path` updated
- P9-L1: `build_harness.sh` — test count updated 55 → 68
- P9-L2: `build_tests.sh` — test count updated 29 → 35
- `generate_lockfile.sh` added to `gui/`

### Architecture
- No dynamic memory allocation anywhere in the stack
- No recursion; all state machines use explicit state variables
- Static buffer management with compile-time size bounds
- Explicit `uds_status_t` return on all public APIs
- Initialization guards on all context structures

---

## [0.9.0] — Phase 8

### Added
- ARDEP fourth ECU example with DFU support
- Extended DTC database with NVM mirror architecture
- Motor controller ECU example with speed/torque DIDs
- ISO-TP consecutive frame and flow control improvements
- Python integration test framework

### Fixed
- Session timeout handling in extended diagnostic session
- Security access delay timer reset on ECU reset

---

## [0.8.0] — Phase 7

### Added
- BMS ECU example with cell voltage and temperature DIDs
- YAML validation in code generator (duplicate DID detection, format checks)
- DTC severity classification
- React GUI configurator initial release

### Fixed
- ISO-TP first frame segmentation for payloads > 4095 bytes
- UDS session persistence across TesterPresent timeouts

---

## [0.5.0] — Phase 5

### Added
- Initial code generator (`tools/codegen.py`) with 3 templates
- DID database with read/write handler registration
- DTC database with status tracking
- Basic ECU reference example
- Unity unit test framework integration
- GitHub Actions CI (4-job initial pipeline)

---

## [0.1.0] — Phase 1

### Added
- Repository structure and architecture documents
- Core UDS server skeleton (0x10, 0x22, 0x3E)
- ISO-TP single frame support
- Zephyr CAN driver binding
- Initial CMakeLists.txt and west.yml
