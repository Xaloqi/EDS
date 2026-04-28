# ARDEP Upgrade Guide

## Replacing `driftregion/iso14229` with the Xaloqi EDS

**Platform:** Mercedes-Benz ARDEP (Automotive Rapid DEvelopment Platform)  
**Board:** `boards/mercedes/ardep` (STM32G4-series, onboard CAN + LIN)  
**Time to complete:** ~2 hours for a working build; ~1 day for full integration

---

## Why Upgrade

ARDEP ships with excellent hardware and a well-structured Zephyr framework. Its
UDS diagnostics layer is built on two MIT-licensed libraries:

- `driftregion/iso14229` — UDS server (C, platform-agnostic)
- `SimonCahill/isotp-c` — ISO-TP transport (C)

These work well for prototyping. For production ECUs, Tier-1 suppliers and OEM
integration reviews ask questions these libraries cannot answer:

| Question | `driftregion/iso14229` | Xaloqi EDS |
|---|---|---|
| ASIL-B safety documentation? | None | ASIL-B safety wrappers, 5-step validation chain, `docs/Safety_Model.md` |
| MISRA C:2012 compliance? | Not stated | the MISRA Deviation Log (Professional tier — xaloqi.com) — 10 deviations, 0 open violations |
| Automated test generation? | Manual tests only | `--test-gen` generates pytest suite from YAML config |
| ISO 26262 work products? | None | MISRA deviation log (Table 1 Method 1b, Table 4 Row 1) |
| AES-128-CMAC security? | Not included | Phase 1 hardening: RFC 4493 AES-CMAC + TRNG seed generation |
| Configuration-driven DIDs? | Code-only | YAML → C code generation; 35 DIDs in one afternoon |
| CI-verified firmware tests? | Not included | pytest suite runs against real compiled firmware in CI |

The upgrade path is surgical — the Zephyr CAN driver integration is identical.
You replace the library, keep your application layer, and gain the safety
documentation your customers will ask for.

---

## What This Example Provides

`examples/ardep_ecu/` is a complete ARDEP I/O Controller configuration:

- **35 DIDs** covering all ARDEP use cases: ECU identity, PowerIO output/input
  state and control, CAN/LIN bus status, ECU health monitoring, calibration
  parameters, and firmware update metadata
- **19 DTCs** covering PowerIO overcurrent, CAN/LIN faults, supply voltage,
  overtemperature, watchdog reset, and firmware update failures
- **FDCAN overlay** (`boards/ardep/ardep.overlay`) mapping the `can0` alias
  to ARDEP's onboard FDCAN1 at 500 kbps ISO 15765-4 default
- **MCUboot-compatible** Kconfig (`boards/ardep/ardep.conf`) — preserves
  ARDEP's DFU over UDS firmware update workflow
- **ASIL-B generated wrappers** — every DID access enforces session, security,
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
| `did_safety_wrappers.h/.c` | ASIL-B 5-step validation before every DID access |
| `uds_init.h/.c` | Stack wiring: databases → session → security → server → ISO-TP |
| `safety_config.h` | Compile-time ASIL-B assertions |
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

ARDEP's primary feature is firmware update over UDS (DFU). The EDS stack
is compatible with ARDEP's MCUboot-based DFU because:

1. **Session compatibility**: ARDEP's UDS Firmware Loader uses Extended and
   Programming sessions — identical to EDS session management (service 0x10)
2. **Security access**: EDS implements the same seed/key security access
   (service 0x27) with AES-CMAC instead of XOR — a drop-in upgrade
3. **MCUboot integration**: The `boards/ardep/ardep.conf` overlay includes
   `CONFIG_BOOTLOADER_MCUBOOT=y` — the EDS application image is MCUboot-signed
4. **DFU download services**: EDS now implements the complete firmware download
   service trio:
   - **0x34 RequestDownload** — validates address range, erases flash, initialises
     the transfer state machine, returns `maxNumberOfBlockLength`
   - **0x36 TransferData** — validates block sequence counter (wraps 0xFF→0x01),
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

## What Changes vs. `driftregion/iso14229`

| Component | `driftregion/iso14229` | Xaloqi EDS |
|---|---|---|
| DID registration | Function pointer per DID | YAML → generated C, ASIL-B wrapper per DID |
| Session management | Caller-managed state | `uds_session_ctx_t` with S3server timer |
| Security access | No AES, no TRNG | AES-128-CMAC, RFC 4493, Zephyr TRNG |
| Error propagation | Return codes | `uds_status_t` + 5-step NRC chain |
| Tests | None | 40+ generated pytest tests (simulator + firmware) |
| MISRA docs | None | the MISRA Deviation Log (Professional tier — xaloqi.com) |
| ISO 26262 docs | None | ASIL-B safety model, MISRA work product |
| CI | None | 6-job GitHub Actions: unit + simulator + firmware + MISRA + Zephyr builds |

---

## Troubleshooting

**`DT_ALIAS(can0)` not resolved:**
Verify `boards/ardep/ardep.overlay` is in the build directory and the FDCAN
node label (`&fdcan1`) matches the ARDEP board DTS. Run `west build -v` to
see which overlays are applied.

**`can_frame_flags_t` undeclared:**
Zephyr 3.x removed these typedefs. The EDS `platform/zephyr/zephyr_can.c` already
uses `(uint8_t)0U` casts — this is fixed. If you see this error, your Zephyr
version may be unpinned. Check `west.yml` pins `zephyr` to `v3.7.0`.

**Security access NRC 0x35 (invalidKey) when testing:**
The placeholder AES keys must match between the tester and firmware. When
running the generated pytest suite, `derive_firmware_key()` uses the same
placeholder keys as `uds_security_algo.c`. After injecting OEM keys, update
`--sec-key-l1` / `--sec-key-l2` pytest options accordingly.

**Build fails on `CONFIG_CAN_STM32FD`:**
Ensure `CONFIG_CAN=y` is set in `prj.conf` and `CONFIG_CAN_STM32FD=y` is
set in `boards/ardep/ardep.conf`. The STM32G4 uses FDCAN (not legacy bxCAN).

---

## Getting Support

For integration questions, open an issue at the EDS repository. For ARDEP
hardware questions, see the [ARDEP documentation](https://mercedes-benz.github.io/ardep/).
