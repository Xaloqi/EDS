# Xaloqi EDS on ARDEP

## Using the Xaloqi EDS as the UDS stack of an ARDEP-based ECU

**Platform:** Mercedes-Benz ARDEP (Automotive Rapid Development Platform, open-source, Apache-2.0)  
**Board:** `boards/mercedes/ardep` (STM32G4-series, onboard CAN + LIN)  
**Evidence:** `examples/ardep_ecu` is compile-verified on `native_sim` only. It has not been built for or run on the `ardep` board, nor inside the ARDEP workspace.

---

## ARDEP's UDS stack, and where EDS differs

ARDEP ships its own UDS server for Zephyr, in three layers
([mercedes-benz/ardep](https://github.com/mercedes-benz/ardep)):

- **`lib/uds`** (Apache-2.0): an event-driven API. An application registers
  check/action handlers per service, statically (`UDS_REGISTER_*` macros) or at
  runtime, on a default instance that sets up CAN and the UDS thread. It
  implements 0x10, 0x11, 0x14, 0x19, 0x22, 0x23, 0x27, 0x28, 0x29, 0x2C, 0x2E,
  0x2F, 0x31, 0x34 to 0x38, 0x3D, 0x3E, 0x85 and LinkControl, and has a ztest
  per service in `tests/lib/uds`.
- **`lib/iso14229`**: a Zephyr wrapper around
  [driftregion/iso14229](https://github.com/driftregion/iso14229) (MIT), which
  embeds [SimonCahill/isotp-c](https://github.com/SimonCahill/isotp-c) for ISO-TP
  over CAN.
- **Firmware loader**: updates the application over UDS or USB DFU, behind MCUboot.

The Xaloqi EDS is a separate UDS stack. An ECU uses one or the other; they are
not layered. Where they differ:

| | ARDEP `lib/uds` | Xaloqi EDS |
|---|---|---|
| Defining DIDs, DTCs and routines | Handlers registered in C | Declared in YAML; codegen emits the tables, wrappers and access rules |
| Tests | Hand-written ztest per service | A pytest suite generated per ECU from the same YAML (simulator and firmware modes) |
| SecurityAccess | Handler hooks; the application supplies seed and key (`samples/uds/src/security_access.c`) | Built-in AES-128-CMAC key derivation with a TRNG seed, with an OEM override hook |
| Safety documentation | None in the repository | ASIL-B-oriented wrappers and a 5-step validation chain (`docs/Safety_Model.md`); safety work products in the Professional tier |
| Transports | CAN (ISO-TP) | CAN, CAN FD and DoIP |
| RTOS | Zephyr | Zephyr and FreeRTOS |

In the other direction, ARDEP implements services this EDS does not
(Authentication 0x29, DynamicallyDefineDataIdentifier 0x2C, RequestFileTransfer
0x38, LinkControl); EDS implements ReadDataByPeriodicIdentifier 0x2A, which
ARDEP's list does not include. Check service coverage against your requirements
before choosing.

Zephyr versions differ too: ARDEP's `west.yml` pins Zephyr v4.4.0, while this
repository's CI gates on Zephyr 3.7 and builds weekly against Zephyr `main`
(report-only). Building the example inside an ARDEP workspace has not been done.

---

## What This Example Provides

`examples/ardep_ecu/` is a complete I/O-controller configuration for the ARDEP board (compile-verified on
`native_sim` only; not built for or run on the `ardep` board):

- **35 DIDs** covering an I/O-controller application: ECU identity, PowerIO output/input
  state and control, CAN/LIN bus status, ECU health monitoring, calibration
  parameters, and firmware update metadata
- **19 DTCs** covering PowerIO overcurrent, CAN/LIN faults, supply voltage,
  overtemperature, watchdog reset, and firmware update failures
- **FDCAN overlay** (`boards/ardep/ardep.overlay`) mapping the `can0` alias
  to ARDEP's onboard FDCAN1 at 500 kbps ISO 15765-4 default
- **MCUboot-compatible** Kconfig (`boards/ardep/ardep.conf`) — preserves
  ARDEP's DFU over UDS firmware update workflow
- **ASIL-B-oriented generated wrappers** — every DID access enforces session, security,
  access permission, and data length validation
- **Auto-generated test suite** — `--test-gen` produces per-DID pytest tests
  that run in simulator mode (no hardware) and against compiled firmware

---

## Prerequisites

1. Working ARDEP Zephyr workspace (follow the
   [ARDEP Getting Started Guide](https://mercedes-benz.github.io/ardep/getting_started/index.html))
2. Python 3.9+ with `pyyaml jinja2 pytest pycryptodome` (`pip install` them)
3. This repository checked out alongside the ARDEP workspace

---

## Step 1 — Add EDS as a West Module

In your `west.yml`, add the EDS repository as a module:

```yaml
manifest:
  projects:
    # ... existing projects (ardep, zephyr, etc.) ...

    - name: embedded-diagnostics-suite
      url: https://github.com/your-org/embedded-diagnostics-suite
      revision: main
      path: eds
```

Then fetch it:

```bash
west update
```

---

## Step 2 — Generate the ARDEP Diagnostic Sources

> **Licensed step.** Producing generated output requires the commercial
> `tools/templates/` Jinja2 templates (Developer/Professional tier, not part of this
> public repo) — see [COMMERCIAL_NOTICE.md](../COMMERCIAL_NOTICE.md). No license?
> `examples/ardep_ecu/generated/` is already committed; skip to the build step.

From the EDS repository root:

```bash
python3 tools/codegen.py \
    --config  examples/ardep_ecu/diagnostics_config.yaml \
    --out     examples/ardep_ecu/generated/ \
    --safety-wrappers \
    --asil-level B \
    --test-gen
```

This produces in `examples/ardep_ecu/generated/`:

| File | Purpose |
|------|---------|
| `generated_config.h` | CAN IDs, timing constants, DID/DTC counts |
| `did_handlers.h/.c` | 35 DID handler stubs (fill with your sensor reads) |
| `did_safety_wrappers.h/.c` | ASIL-B-oriented 5-step validation before every DID access |
| `uds_init.h/.c` | Stack wiring: databases → session → security → server → ISO-TP |
| `safety_config.h` | Compile-time ASIL-B-oriented assertions |
| `tests/conftest.py` | pytest fixtures (simulator + firmware modes) |
| `tests/test_firmware_services.py` | 40+ firmware-backed integration tests |
| `tests/test_did_*.py` | Per-DID exhaustive test files |

---

## Step 3 — Build for ARDEP Hardware

```bash
cd <ardep-workspace>

west build -b ardep eds/examples/ardep_ecu \
    -- -DDIAG_SKIP_CODEGEN=ON
```

Zephyr's board overlay auto-discovery will pick up
`eds/examples/ardep_ecu/boards/ardep/ardep.overlay` and
`eds/examples/ardep_ecu/boards/ardep/ardep.conf` automatically when the
source directory is `eds/examples/ardep_ecu`.

Flash to the ARDEP board:

```bash
west flash
```

---

## Step 4 — Connect a Diagnostic Tester

With the ARDEP board connected to a CAN bus at 500 kbps, use any ISO 15765-4
compatible tool. With the ARDEP CAN-USB bridge sample running:

```bash
# Set up vcan0 (Linux):
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set vcan0 up

# Run the ARDEP CAN-USB bridge to forward frames to/from vcan0
# (see ARDEP samples/can_usb_bridge)

# Then run the generated test suite:
cd eds/examples/ardep_ecu/generated/tests
pytest test_firmware_services.py -v
```

Or test manually with `isotpsend` / `isotprecv`:

```bash
# Read VIN (DID 0xF190) in Default Session:
echo "03 22 F1 90" | isotpsend -s 7DF -d 7E8 vcan0
isotprecv -s 7E8 -d 7DF vcan0

# Read PowerIO output state (DID 0x2001):
echo "03 22 20 01" | isotpsend -s 7DF -d 7E8 vcan0
```

---

## Step 5 — Wire Your Application Sensors

Open `examples/ardep_ecu/src/main.c`. Each DID handler has a `TODO` comment
with the exact Zephyr API call needed. Example:

```c
/* Before (stub): */
uds_status_t ardep_read_ecu_supplyvoltage_mv(uint8_t *buf)
{
    /* TODO: Read from ADC channel connected to supply voltage divider. */
    buf[0] = (uint8_t)(s_supply_mv >> 8U);
    buf[1] = (uint8_t)(s_supply_mv & 0xFFU);
    return UDS_STATUS_OK;
}

/* After (production): */
uds_status_t ardep_read_ecu_supplyvoltage_mv(uint8_t *buf)
{
    int32_t mv;
    adc_read_vsupply(&mv);             /* your ADC driver call */
    s_supply_mv = (uint16_t)mv;
    buf[0] = (uint8_t)(s_supply_mv >> 8U);
    buf[1] = (uint8_t)(s_supply_mv & 0xFFU);
    return UDS_STATUS_OK;
}
```

---

## Step 6 — Inject OEM Security Keys

Before shipping, replace the placeholder AES-128 keys in `main.c`:

```c
/* In diag_security_algo_init(): */

/* REPLACE THIS: */
LOG_WRN("[SEC] Using placeholder AES keys — inject OEM keys before production.");

/* WITH THIS (example using Zephyr NVS): */
uint8_t key_l1[16], key_l2[16];
nvs_read(&ardep_fs, NVS_ID_DIAG_KEY_L1, key_l1, sizeof(key_l1));
nvs_read(&ardep_fs, NVS_ID_DIAG_KEY_L2, key_l2, sizeof(key_l2));
uds_security_algo_set_level_key(0x01U, key_l1);
uds_security_algo_set_level_key(0x03U, key_l2);
memset(key_l1, 0, sizeof(key_l1));    /* zeroize after use */
memset(key_l2, 0, sizeof(key_l2));
```

---

## Step 7 — Run the Full Test Suite

```bash
# Simulator mode (no hardware):
cd eds/examples/ardep_ecu/generated/tests
pip install pytest pycryptodome
pytest . -v --can-interface=simulator

# Firmware mode (against compiled C stack, no ARDEP hardware needed):
cd eds
bash build_harness.sh --fast
cd examples/ardep_ecu/generated/tests
pytest test_firmware_services.py -v
```

---

## Preserving ARDEP's DFU Workflow

ARDEP's firmware loader updates the application over UDS (DFU) behind MCUboot.
What the EDS example shares with it, and what it does not:

1. **Session**: the loader switches to the Programming session
   (`firmware_loader/src/uds_session.c`); the example's download services also
   require Programming session (service 0x10).
2. **Security access and routine IDs differ**: ARDEP's flash tool
   (`scripts/runner/ardep-uds.py`) erases slot0 with routine `0xFF00`, then runs
   0x34, 0x36 and 0x37, and sends no SecurityAccess. This example erases with
   `0xFF10`, verifies with `0xFF11` and requires Security Level 1, so that tool
   would need adapting. This has not been tried.
3. **MCUboot integration**: The `boards/ardep/ardep.conf` overlay includes
   `CONFIG_BOOTLOADER_MCUBOOT=y` — the EDS application image is MCUboot-signed
4. **DFU download services**: EDS now implements the complete firmware download
   service trio:
   - **0x34 RequestDownload** — validates address range, erases flash, initialises
     the transfer state machine, returns `maxNumberOfBlockLength`
   - **0x36 TransferData** — validates block sequence counter (rolls over 0xFF→0x00),
     accumulates data, calls `flash_write_cb` per block, maintains running CRC-32
   - **0x37 RequestTransferExit** — validates optional CRC-32 record, calls
     `flash_verify_cb`, resets transfer state to IDLE

**DFU Integration**: Call `zephyr_flash_ops_init()` from `main.c` before
`uds_generated_init()` to register the MCUboot secondary-slot flash driver.
The complete DFU sequence is:

```
1. 0x31 RID 0xFF10 startRoutine  — EraseApplicationFlash (prerequisite)
2. 0x34 RequestDownload          — open transfer session
3. 0x36 TransferData (×N)        — send firmware blocks
4. 0x37 RequestTransferExit      — commit and verify
5. 0x31 RID 0xFF11 startRoutine  — VerifyApplicationImage
```

All three download services require **Programming session** + **Security Level 1
unlock** (enforced by the ACL table). The `boards/ardep/ardep.conf` already
enables `CONFIG_BOOTLOADER_MCUBOOT=y` and `CONFIG_FLASH_MAP=y`.

---

## Troubleshooting

**`DT_ALIAS(can0)` not resolved:**
Verify `boards/ardep/ardep.overlay` is in the build directory and the FDCAN
node label (`&fdcan1`) matches the ARDEP board DTS. Run `west build -v` to
see which overlays are applied.

**`can_frame_flags_t` undeclared:**
Zephyr 3.x removed these typedefs. The EDS `platform/zephyr/zephyr_can.c` already
uses `(uint8_t)0U` casts. If you still see this error, compare your workspace's
Zephyr revision (`zephyr` in `west.yml`) with the versions this repository's CI
builds against (see the README).

**Security access NRC 0x35 (invalidKey) when testing:**
The placeholder AES keys must match between the tester and firmware. When
running the generated pytest suite, `derive_firmware_key()` uses the same
placeholder keys as `uds_security_algo.c`. After injecting OEM keys, update
`--sec-key-l1` / `--sec-key-l2` pytest options accordingly.

**Build fails with "attempt to assign the value 'y' to the undefined symbol CAN_STM32FD":**
Remove that line. No current Zephyr defines `CAN_STM32FD`; the STM32G4's FDCAN
driver (`CAN_STM32_FDCAN`) is selected from devicetree once `CONFIG_CAN=y` is
set. `boards/ardep/ardep.conf` sets only `CONFIG_CAN=y`.

---

## Getting Support

For integration questions, open an issue at the EDS repository. For ARDEP
hardware questions, see the [ARDEP documentation](https://mercedes-benz.github.io/ardep/).
