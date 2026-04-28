# BMS ECU Example — Xaloqi EDS

**Battery Management System — 24 DIDs · 10 DTCs · 5 routines · AES-128 SecurityAccess**

This example demonstrates Xaloqi EDS configured for a **generic automotive passenger-car
Battery Management System (BMS)** — 96-cell series Li-Ion pack, 400 V nominal, with cell
voltage monitoring, pack current measurement, thermal management DIDs, contactor control
routines, and full DTC persistence.

It is the primary reference for EV drivetrain ECU teams on Zephyr RTOS.

---

## What this example contains

```
bms_ecu/
├── diagnostics_config.yaml      24 DIDs, 10 DTCs, 5 routines, security level 1
├── src/main.c                   Application entry point + DID handler stubs
├── CMakeLists.txt               Zephyr west build configuration
├── prj.conf                     Kconfig: CAN + ISO-TP + UDS enabled
├── boards/native_sim/           native_sim overlay + config
└── generated/                   Pre-built codegen output (C/H + 36-test suite)
```

**DID highlights:**
- Cell group voltages (8 × 2 bytes) — `0xDB00`–`0xDB04`
- Pack voltage / current / temperature — `0xD900`–`0xD911`
- SoC / SoH — `0xDD00`–`0xDD03`
- Contactor state, isolation status — `0xDE00`–`0xDE02`
- OBD2 standard DIDs — `0xF187`, `0xF189`, `0xF18C`, `0xF190`

**Routines:**
- `0xBB00` Cell Balancing Cycle
- `0xBB01` Pack Isolation Test
- `0xBB02` Contactor Self-Test
- `0xBB10` SoC Recalibration
- `0xBB11` SoH Reset

---

## Availability

This example is included with **Developer** and **Professional** licenses.

**[Purchase at xaloqi.com →](https://xaloqi.com)**

After purchase, extract your delivery ZIP into the repo root and this directory
will be populated with the full example, including `diagnostics_config.yaml`,
`src/`, and the pre-generated test suite.

---

## Quick preview

The BMS example `diagnostics_config.yaml` begins with:

```yaml
schema_version: 1
metadata:
  ecu_name: "BatteryManagementECU"
  version:  "1.0.0"

timing:
  p2_server_max_ms:      25
  p2_star_server_max_ms: 5000
  s3_server_timeout_ms:  5000

security:
  levels:
    - level: 1
      algorithm: aes128_cmac
      seed_length: 4

dids:
  - id: "0xD900"
    name: "PackVoltage"
    access: [read]
    min_session: default
    read_security_level: 0
    data_length: 4        # uint32, millivolts

  - id: "0xD901"
    name: "PackCurrent"
    access: [read]
    min_session: default
    read_security_level: 0
    data_length: 4        # int32, milliamps

  # ... 22 more DIDs
```

The full config, source, and generated test suite are delivered with the license.

---

See also: [`examples/basic_ecu/`](../basic_ecu/) — free community example showing
the same pattern with 5 DIDs and 2 DTCs.
