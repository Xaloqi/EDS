# ARDEP I/O Controller Example — Xaloqi EDS

**An I/O controller diagnostics configuration for the ARDEP board — 35 DIDs · 19 DTCs · 6 routines**

ARDEP is the **Automotive Rapid Development Platform**: Mercedes-Benz's open-source
(Apache-2.0) hardware and Zephyr software platform ([mercedes-benz/ardep](https://github.com/mercedes-benz/ardep);
board `boards/mercedes/ardep`, STM32G474). This example shows Xaloqi EDS configured for
an I/O-controller application on that board and its PowerIO Shield: discrete inputs,
analogue sensors, high-side outputs, CAN/LIN status and firmware-update metadata.

Evidence: compile-verified on `native_sim`, and for the `ardep` board in ARDEP's own west
workspace (CI job `zephyr-ardep`: ARDEP pinned to a commit, its Zephyr v4.4.0). It has not
been run on an ARDEP board.

Building for the board, from an ARDEP workspace (`west init -m https://github.com/mercedes-benz/ardep`,
`west update`) with Zephyr SDK 1.x:

```bash
west build -b ardep <path-to-eds>/examples/ardep_ecu -- \
  -DDIAG_SKIP_CODEGEN=ON \
  -DCONFIG_NEWLIB_LIBC=n -DCONFIG_EDS_REQUIRE_NEWLIB=n \
  -DEXTRA_CONF_FILE=<path-to-eds>/examples/ardep_ecu/boards/ardep/ardep.conf \
  -DDTC_OVERLAY_FILE=<path-to-eds>/examples/ardep_ecu/boards/ardep/ardep.overlay
```

The two `NEWLIB` settings accept picolibc: Zephyr SDK 1.x ships no newlib (EDS#498). This
builds the application image only. Flashing it on the board goes through ARDEP's MCUboot,
which this example does not build yet.

---

## What this example contains

```
ardep_ecu/
├── diagnostics_config.yaml      35 DIDs, 19 DTCs, 6 routines
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

Free to build, run and study. The example is Apache-2.0; the EDS runtime it links is
GPL v2. Regenerating `generated/` from a changed `diagnostics_config.yaml` needs a
**Developer** licence ([xaloqi.com](https://xaloqi.com)); shipping the runtime in
proprietary firmware needs a commercial licence. See the repository README, "Open-core".

---

See also: [`examples/basic_ecu/`](../basic_ecu/) — free community example.
