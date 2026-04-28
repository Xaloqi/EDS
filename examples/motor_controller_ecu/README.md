# Motor Controller ECU Example — Xaloqi EDS

**Traction Motor Controller — 27 DIDs · 6 DTCs · 6 routines · ASIL-B**

This example demonstrates Xaloqi EDS configured for an **automotive traction motor
controller ECU** — permanent magnet synchronous motor (PMSM), FOC inverter, with
torque / speed / temperature DIDs, inverter health routines, and thermal protection DTCs.

The reference architecture for EV powertrain ECU teams.

---

## What this example contains

```
motor_controller_ecu/
├── diagnostics_config.yaml      27 DIDs, 6 DTCs, 6 routines
├── src/main.c                   Application entry point + DID stubs
├── CMakeLists.txt               Zephyr west build (native_sim + nucleo_h743zi)
├── prj.conf
├── boards/native_sim/
└── generated/                   Pre-built codegen output (C/H + test suite)
```

**DID highlights:**
- Motor torque / speed / power — `0xE001`–`0xE005`
- Phase currents, DC link voltage — `0xE101`–`0xE105`
- Motor / inverter / coolant temperatures — `0xE201`–`0xE203`
- FOC state: flux / d-q currents / modulation — `0xE301`–`0xE305`
- Efficiency, energy counters — `0xE401`–`0xE402`
- Fault codes and protection thresholds — `0xE501`–`0xE503`

**Routines:**
- `0xCC00` FOC Calibration
- `0xCC01` Phase Balance Check
- `0xCC02` Encoder Alignment
- `0xCC10` Thermal Derating Reset
- `0xCC11` Inverter Self-Test
- `0xCC12` Torque Limiter Reset

---

## Availability

Included with **Developer** and **Professional** licenses.

**[Purchase at xaloqi.com →](https://xaloqi.com)**

---

See also: [`examples/basic_ecu/`](../basic_ecu/) — free community example.
