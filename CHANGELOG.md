# Changelog

All notable changes to the Xaloqi EDS are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning follows [Semantic Versioning](https://semver.org/).

---
##  [1.5.0] — TestLab integration + testgen refactor — 2026-05-07
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

## [1.4.0] — Job Engine + CI expansion — 2026-04-30

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

## [1.3.0] — Platform housekeeping + FreeRTOS API — 2026-04-20

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

## [1.3.0] — SafeBoot + Sensor + Robotics Examples — 2026-04-15

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

## [1.2.0] — FreeRTOS Platform Support — 2026-04-15

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

## [1.1.0] — Commercial Readiness — 2026-03-26

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
- Payment platform: Polar.sh.
- Legal entity: Raul Latorre Fortes, trading as Xaloqi, Frankfurt am Main, Germany.

## [1.1.0] — Layer 4 + Layer 5 Complete — 2026-03-18

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

## [1.0.0] — Phase 9 Complete — 2026-03-11

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

## [0.9.0] — Phase 8 — 2026-02-15

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

## [0.8.0] — Phase 7 — 2026-01-20

### Added
- BMS ECU example with cell voltage and temperature DIDs
- YAML validation in code generator (duplicate DID detection, format checks)
- DTC severity classification
- React GUI configurator initial release

### Fixed
- ISO-TP first frame segmentation for payloads > 4095 bytes
- UDS session persistence across TesterPresent timeouts

---

## [0.5.0] — Phase 5 — 2025-11-10

### Added
- Initial code generator (`tools/codegen.py`) with 3 templates
- DID database with read/write handler registration
- DTC database with status tracking
- Basic ECU reference example
- Unity unit test framework integration
- GitHub Actions CI (4-job initial pipeline)

---

## [0.1.0] — Phase 1 — 2025-09-01

### Added
- Repository structure and architecture documents
- Core UDS server skeleton (0x10, 0x22, 0x3E)
- ISO-TP single frame support
- Zephyr CAN driver binding
- Initial CMakeLists.txt and west.yml
