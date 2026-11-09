# BMS ECU Example — Xaloqi EDS

**UDS diagnostics for a battery management system — 24 DIDs · 10 DTCs · 5 routines · Zephyr, `native_sim`**

This example is the **main controller ECU of a passenger-car BMS** (96s Li-Ion pack, 8 groups of 12
cells): pack voltage, current and power, four cell-group voltages, four temperatures, SoC/SoH,
contactor and isolation state, two writable protection thresholds, ten DTCs and five routines, over
ISO 14229 UDS on CAN. It runs on `native_sim` against a **simulated pack** and you can query it
from the host with any UDS tester.

**What is verified, and what is not:** this has only run on `native_sim` (see
[Test boundary](#test-boundary)). The pack is a fixed set of boot values in `src/bms_sim.c`; no cell
monitor, current sensor, contactor or isolation monitor has been connected.

---

## What this example contains

```
bms_ecu/
├── diagnostics_config.yaml      24 DIDs, 10 DTCs, 5 routines
├── src/main.c                   application entry point, diag task
├── src/bms_sim.c                simulated pack behind the 20 live DIDs
├── boards/native_sim/           loopback overlay and config
├── CMakeLists.txt
└── generated/                   committed codegen output (nothing edited by hand)
```

**DIDs (24)**: the session column is the minimum session; writes need security level 1.

| DID | Name | Encoding | Session | Source |
|---|---|---|---|---|
| 0xF190 | VehicleIdentificationNumber | 17 bytes ASCII | default | generated (fixed value `BMSEXAMPLE0000001`) |
| 0xF18C | ECUSerialNumber | 8 bytes ASCII | default | generated (fixed value `BMS00001`) |
| 0xF187 | VehicleManufacturerSparePartNumber | 11 bytes ASCII, read/write | extended | generated store, `00000000000` at boot |
| 0xF189 | ECUSoftwareVersionNumber | 4 bytes | default | generated (fixed value `01 00 00 00`) |
| 0xDB00 | BMS_PackVoltage_mV | uint32 BE, mV | default | **live**, `bms_sim.c`: 357 120 mV |
| 0xDB01 | BMS_PackCurrent_100mA | int16 BE, 0.1 A (+ = discharge) | default | **live**: 0 |
| 0xDB02 | BMS_PackPower_10W | int16 BE, 10 W | default | **live**: 0 |
| 0xDB03 | BMS_ContactorState | uint8 bitmask | default | **live**: `00`, all open |
| 0xDB04 | BMS_InsulationResistance_kOhm | uint16 BE, kΩ | default | **live**: `FFFF`, IMD not started |
| 0xDC01–0xDC04 | BMS_CellGroup1–4_Voltage_mV | uint16 BE, mV | default | **live**: 44 640 mV each |
| 0xDD00 | BMS_MaxCellTemperature_degC | uint8, raw = °C + 40 | default | **live**: `41` = 25 °C |
| 0xDD01 | BMS_MinCellTemperature_degC | uint8, raw = °C + 40 | default | **live**: 25 °C |
| 0xDD02 | BMS_CoolantInletTemperature_degC | uint8, raw = °C + 40 | default | **live**: 25 °C |
| 0xDD03 | BMS_BMSBoardTemperature_degC | uint8, raw = °C + 40 | default | **live**: 25 °C |
| 0xDE00 | BMS_StateOfCharge_04pct | uint8, 0.4 %/LSB | default | **live**: `7D` = 50 % |
| 0xDE01 | BMS_StateOfHealth_04pct | uint8, 0.4 %/LSB | extended | **live**: `FA` = 100 % |
| 0xDE02 | BMS_BalancingStateBitmask | uint8 bitmask | extended | **live**: `00` |
| 0xD900 | BMS_FaultFlagsBitmask | uint16 BE bitmask | default | **live**: `0000` |
| 0xD901 | BMS_FullChargeCapacity_Wh | uint32 BE, Wh | extended | **live**: 75 000 Wh |
| 0xD910 | BMS_OVThreshold_mV | uint16 BE, mV, read/write | extended | **live**: 4 200 mV; writes accepted in 3 800–4 350 |
| 0xD911 | BMS_UVThreshold_mV | uint16 BE, mV, read/write | extended | **live**: 2 800 mV; writes accepted in 2 500–3 200 |

**DTCs (10):** 0xD10100 cell group overvoltage, 0xD10200 undervoltage, 0xD10300 cell
over-temperature on discharge, 0xD10400 on charge, 0xD10500 pack overcurrent, 0xD10600 isolation
fault, 0xD10700 contactor weld, 0xD10800 current sensor fault, 0xD10900 BMS MCU fault, 0xC00100
CAN communication loss. None is driven by the simulation.

**Routines (5):**

| Routine | Name | Session | Security | Sub-functions |
|---|---|---|---|---|
| 0xBB00 | BMS_SelfTest | extended | none | start, results |
| 0xBB01 | BMS_ForcePassiveBalance | extended | level 1 | start, stop, results |
| 0xBB02 | BMS_ContactorFunctionalTest | extended | level 1 | start, results |
| 0xBB10 | BMS_ResetSoCToFullCharge | programming | level 1 | start |
| 0xBB11 | BMS_ClearFaultHistory | programming | level 1 | start |

These are the generated stubs.

**Timing:** P2 server 50 ms, P2* 5 000 ms, S3 5 000 ms (the ECU answers `10 03` with
`50 03 00 32 01 F4`). Addressing: 0x7E0 physical request (0x7DF functional), response 0x7E8.

### The simulated pack

The 20 live DIDs (0xDB00–0xDB04, 0xDC01–0xDC04, 0xDD00–0xDD03, 0xDE00–0xDE02, 0xD900, 0xD901,
0xD910, 0xD911) are `source: app` in the YAML: codegen emits no body for them and `src/bms_sim.c`
defines `did_read_<name>()` / `did_write_<name>()`. The four identity DIDs keep their generated
fixed values (`default_value` in the YAML). The pack values are fixed at boot, and only the two thresholds change (a UDS write). To use
real hardware, replace `bms_sim.c`.

---

## Building

Two `native_sim` variants. Both pass `-DDIAG_SKIP_CODEGEN=ON` (the committed `generated/` is used,
so no licence or `pyyaml`/`jinja2` is needed).

**Host-reachable (SocketCAN) — use this to query it from the host:**

```bash
EDS=<eds>
west build -b native_sim $EDS/examples/bms_ecu \
  -- -DDIAG_SKIP_CODEGEN=ON \
     "-DEXTRA_CONF_FILE=$EDS/boards/native_sim/native_sim.conf;$EDS/boards/native_sim/native_sim_realcan.conf" \
     "-DDTC_OVERLAY_FILE=$EDS/boards/native_sim/native_sim.overlay;$EDS/boards/native_sim/native_sim_realcan.overlay"
```

`can0` is then bound to the host CAN interface **`zcan0`**.

**Loopback — compile check only:** `-DDTC_OVERLAY_FILE=$EDS/examples/bms_ecu/boards/native_sim/native_sim.overlay`
and no `EXTRA_CONF_FILE`. `can_loopback0` talks only to itself, so no tester can reach it. CI builds
this one in the Zephyr examples matrix.

## Run it and query it from the host

You need a CAN interface named `zcan0` (a virtual one is enough) and the kernel's `vcan` and
`can_raw` modules. Without root, a user + network namespace gives you one; run the ECU and the tester
**inside the same namespace**:

```bash
unshare -rn bash -c '
  ip link add zcan0 type vcan && ip link set zcan0 up
  ./build/zephyr/zephyr.exe &
  sleep 1
  stdbuf -oL isotprecv -l -s 7E0 -d 7E8 zcan0 &
  sleep 0.3
  echo "22 DB 00" | isotpsend -s 7E0 -d 7E8 zcan0    # pack voltage
  echo "22 DE 00" | isotpsend -s 7E0 -d 7E8 zcan0    # state of charge
  sleep 1; kill %1 %2'
```

Things to try, in order:

| Request | What you see |
|---|---|
| `22 DB 00` | `62 DB 00 00 05 73 00` = 357 120 mV |
| `22 DC 01` | `62 DC 01 AE 60` = 44 640 mV |
| `22 DE 00` | `62 DE 00 7D` = 125 × 0.4 % = 50 % |
| `22 F1 90` | `62 F1 90` + `BMSEXAMPLE0000001` (a multi-frame response) |
| `22 DE 01` in the default session | negative response `7F 22 31`, request out of range |
| `10 03`, then `22 DE 01` | `62 DE 01 FA` = 100 % |
| `22 D9 10` (extended) | `62 D9 10 10 68` = 4 200 mV |
| `2E D9 10 10 90` (extended, no security) | `7F 2E 33`, security access denied |
| `27 01`, `27 02 <key>`, then the `2E` again | `67 02`, then `6E D9 10`; `22 D9 10` reads back `62 D9 10 10 90` (4 240 mV) |
| `2E D9 10 0E 10` after unlocking (3 600 mV, below 3 800) | `7F 2E 31`, request out of range; the value stays 4 240 mV |

The key for `27 02` is the AES-128-CMAC of the seed under the **placeholder** key every example
ships with (`xaloqi.tester` derives it); the ECU logs "Placeholder AES keys active". Production
firmware must inject real keys.

`scripts/smoke_bms_ecu.py` reads the 20 live DIDs and the three read-only identity DIDs and compares them with the values above; CI runs
it (`example-bms-runtime`):

```bash
unshare -rn bash -c 'ip link add zcan0 type vcan && ip link set zcan0 up &&
  python3 scripts/smoke_bms_ecu.py build/zephyr/zephyr.exe zcan0'
```

## Regenerating

`generated/` is committed. If you change `diagnostics_config.yaml`, regenerate with the Developer
licence:

```bash
python3 tools/codegen.py --config examples/bms_ecu/diagnostics_config.yaml \
                         --out    examples/bms_ecu/generated/ \
                         --safety-wrappers --asil-level B --test-gen --no-manifest
```

`src/bms_sim.c` is not touched by codegen. `generated/tests/` is a pytest suite of 31 files (one
per DID and routine, plus services and firmware services; 334 tests collected). It runs against the
bundled simulator by default.

---

## Test boundary

- **Verified:** builds on `native_sim` (loopback and SocketCAN variants); over `vcan` the 20 live
  DIDs and the identity DIDs return the `bms_sim.c` values above (`scripts/smoke_bms_ecu.py`, CI job
  `example-bms-runtime`).
- **Not verified:** any hardware. No cell monitor, current sensor, NTC, contactor or isolation
  monitor has been connected and no MCU board has run this example. The values are a model, not
  measurements. The routines and the ten DTCs are generated stubs and are not exercised by the
  smoke; the `0x2E` rows above were run by hand, not in CI.

---

## Availability

Free to build, run and study. The example is Apache-2.0; the EDS runtime it links is
GPL v2. Regenerating `generated/` from a changed `diagnostics_config.yaml` needs a
**Developer** licence ([xaloqi.com](https://xaloqi.com)); shipping the runtime in
proprietary firmware needs a commercial licence. See the repository README, "Open-core".
