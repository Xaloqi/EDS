# ARDEP I/O Controller Example — Xaloqi EDS

**Automotive Remote Digital/Analogue I/O Controller — 32 DIDs · 8 DTCs · 6 routines**

This example demonstrates Xaloqi EDS configured for an **ARDEP (Automotive Remote
Digital/Analogue Processing) zone controller** — a general-purpose I/O expansion ECU
managing discrete inputs, analogue sensors, PWM outputs, and relay control.

The reference for body-domain and zone-controller ECU teams.

---

## What this example contains

```
ardep_ecu/
├── diagnostics_config.yaml      32 DIDs across 6 I/O banks, 8 DTCs, 6 routines
├── src/main.c
├── CMakeLists.txt
├── prj.conf
├── boards/native_sim/
└── generated/                   Pre-built codegen output (C/H + 45-test suite)
```

**DID highlights:**
- Digital input banks A–D (8 channels each) — `0x2001`–`0x2008`
- Analogue input banks (12-bit, mV) — `0x2101`–`0x2104`
- PWM output state and duty cycle — `0x2201`–`0x2212`
- Relay status and cycle count — `0x2301`–`0x2306`
- Power supply rails and ground offsets — `0x2401`–`0x2404`
- ECU temperature and supply voltage — `0x2501`–`0x2503`

**Routines:**
- `0xFF00` Digital Output Self-Test
- `0xFF01` Analogue Calibration Cycle
- `0xFF10` Relay Actuate and Verify
- `0xFF11` PWM Frequency Sweep
- `0xFF20` Power Supply Check
- `0xFF21` Communication Loopback

---

## Availability

Included with **Developer** and **Professional** licenses.

**[Purchase at xaloqi.com →](https://xaloqi.com)**

---

See also: [`examples/basic_ecu/`](../basic_ecu/) — free community example.
