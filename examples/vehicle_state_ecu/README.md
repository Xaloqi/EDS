# vehicle_state_ecu — Xaloqi EDS example (Zephyr, native_sim)

**EDS version:** v1.16.1 · **Transport:** CAN / ISO-TP (loopback on native_sim)
**DIDs:** 5 · **DTCs:** 2 · **Routines:** 2

---

## What this example demonstrates

A Zephyr ECU example shaped like [AGL SoDeV](https://wiki.automotivelinux.org/sodev)'s own
reference application, with the diagnostics layer SoDeV leaves to you — build it and run
the campaign on your laptop.

SoDeV's reference demo application ([`sodev-demo-led-controller`](https://github.com/automotive-grade-linux/sodev-demo-led-controller))
reads two vehicle-state inputs, vehicle speed and engine RPM, and drives a WS2812 LED
strip. It has no diagnostics at all. This example takes the same two inputs and gives
them an ISO 14229 face:

- Both signals as **DIDs** with sane scaling (`0x0C00` engine speed, `0x0D00` vehicle
  speed — the same OBD-II PID numbering [`examples/basic_ecu`](../basic_ecu/) already
  uses for engine speed).
- Two **SAE J2012 DTCs** on faults those two signals can actually produce — one
  availability fault, one cross-signal plausibility fault.
- A **RoutineControl actuation path** (`0xC100` / `0xC101`) over an indicator output
  stage that stands in for the LED strip.
- A **TestLab campaign** (`campaigns/vehicle_state.yaml`) exercising all of it.

One `diagnostics_config.yaml` is both the firmware's source (via `tools/codegen.py`) and
the campaign's ECU description (via `testlab-run --config`).

**Xaloqi EDS is an independent ISO 14229 diagnostics stack.** It is not part of, built
on, validated by, compatible-certified for, or endorsed by Automotive Grade Linux,
SoDeV, or the Linux Foundation; those names are used nominatively to describe the
application shape this example mirrors. Written clean, not derived from any SoDeV
source. This example ships no Raspberry Pi Pico target — EDS has no RP2040/RP2350 port.
`native_sim` is the tryout: no board, no Yocto, no hypervisor.

---

## DIDs

| ID | Name | Bytes | Encoding | Access |
|---|---|---|---|---|
| `0xF190` | VehicleIdentificationNumber | 17 | ASCII | read |
| `0xF18C` | ECUSerialNumber | 4 | ASCII | read |
| `0xF189` | ECUSoftwareVersionNumber | 4 | major.minor.patch.build | read |
| `0x0C00` | VehicleState_EngineSpeed_rpm | 2 | uint16 BE, 1 rpm/bit | read, write |
| `0x0D00` | VehicleState_VehicleSpeed_kph | 2 | uint16 BE, 0.1 km/h/bit | read, write |

**Why the vehicle-state DIDs are writable:** this ECU doesn't own the sensors — on a
vehicle it receives both signals over the bus. `native_sim` has no bus, so writing these
DIDs (behind a SecurityAccess level 1 / AES-128-CMAC challenge) is how a test campaign
*injects* the ECU's inputs and provokes the two DTC monitors below. In a production
configuration, remove `write` from `access` in `diagnostics_config.yaml`, or compile the
injection path out of the build.

## DTCs

| Code | Decodes as | Trigger |
|---|---|---|
| `0xC10000` | U0100-00 — lost communication with engine control module | `VehicleState_EngineSpeed_rpm` pinned above 8000 rpm for ≥ 500 ms |
| `0x050064` | P0500-64 — vehicle speed signal implausible against engine speed | Speed > 5.0 km/h while engine reads exactly 0 rpm, for ≥ 1000 ms |

Both are real SAE J2012 codes. Both triggers and the monitor loop that evaluates them
live in `src/vehicle_state.c` — see that file's header comment for why U0100 is modelled
as an out-of-range pinned value rather than "the value stopped changing": these two DIDs
are tester-injected, so a value holding steady between writes is normal, not a fault.

## Routines — the actuation path

| RID | Name | Behaviour |
|---|---|---|
| `0xC100` | IndicatorOutputSelfTest | Asynchronous: `start` returns immediately (`0x01`, in progress) and walks the indicator through all four patterns over the next ~400 ms; poll `requestRoutineResults` for `[result, patterns_tested_bitmask, current_state]`. |
| `0xC101` | IndicatorLampCheck | `start` takes over the output and holds the all-on lamp-check pattern (returns the pattern byte); `stop` releases it and returns the restored application state. Released automatically if the session drops back to Default — including on an S3server timeout — so a diagnostic client that disappears mid-test doesn't leave the output stuck. |

The indicator itself has no DID or hardware output on `native_sim` — watch `LOG_INF`
lines on the console (`./build/zephyr/zephyr.exe`); every state transition is logged.

RIDs are in the ISO 14229-1 vehicleManufacturerSpecific range, like
[`examples/motor_controller_ecu`](../motor_controller_ecu/)'s `0xCC0x` — not
`basic_ecu`'s `0xFF00`/`0xFF01`, which sit near ISO's reserved EraseMemory /
CheckProgrammingDependencies RIDs.

**Why RoutineControl and not IOControl (0x2F):** `core/uds_services/service_0x2F.c`
exists in the runtime, but no example in this repo wires it up — it needs a
`did_io_control_cb_fn` registered against a DID, and there's no YAML/codegen path to do
that (you'd hand-register it in `main.c`, against a core API not designed for it). More
decisively: the free `xaloqi-tester` campaign runner has no `io_control` action at all
(see `xaloqi-testlab-core`'s `VALID_ACTIONS`) — a 0x2F-based actuation path couldn't be
exercised by the free campaign this example ships.

---

## Building

```bash
west build -b native_sim examples/vehicle_state_ecu
./build/zephyr/zephyr.exe
```

That's the whole command — `boards/native_sim.conf` and `boards/native_sim.overlay` are
picked up automatically by Zephyr's build system, no `-DEXTRA_CONF_FILE=...` needed.

`generated/` is pre-committed (this build does **not** run codegen by default —
`DIAG_SKIP_CODEGEN=ON`, see `CMakeLists.txt`), so this works without `pyyaml`/`jinja2` or
the commercial `tools/templates/`. If you edit `diagnostics_config.yaml`, regenerate
with:

```bash
python3 tools/codegen.py --config examples/vehicle_state_ecu/diagnostics_config.yaml \
                          --out    examples/vehicle_state_ecu/generated/ \
                          --safety-wrappers --asil-level B --no-manifest
```

then reapply the four `[APP HOOK]` bodies in `generated/routine_handlers.c` (a configure-time
check fails the build loudly if you forget — see `CMakeLists.txt`).

## Running the campaign

```bash
pip install xaloqi-tester
testlab-run --config   examples/vehicle_state_ecu/diagnostics_config.yaml \
            --campaign examples/vehicle_state_ecu/campaigns/vehicle_state.yaml \
            --job      vehicle_state_validation \
            --virtual \
            --json     reports/virtual.json
```

`--virtual` runs the free tier's in-process virtual ECU — no build, no binary, nothing
beyond `pip install`. `vehicle_state_validation` is written to run unchanged against
the built `native_sim` firmware over a real transport too — identity reads, signal
inject-then-read-back on both DIDs, both routines, and the DTC read/clear/re-read
round-trip, nothing in the campaign is simulator-specific. **Confirmed**: both the
virtual-ECU run above and the real firmware over real SocketCAN (`vcan0`), 22/22 steps.

The campaign's second job, `vehicle_state_fault_injection`, actually provokes both DTC
monitors and watches them clear — that needs the real firmware's monitor loop, not the
virtual ECU, so it's firmware-only (run it with EDS's own `tools/jobrunner.py` or
TestLab Pro's SocketCAN transport against a built `native_sim` binary on `vcan0`).
**Confirmed against real firmware over real `vcan0`**: both monitors fired and healed
for real — `DTC P0500-64 set: speed=450 (0.1 km/h) with engine_rpm=0` /
`DTC U0100-00 set: engine_rpm=9000 exceeds plausible max`, both followed by their
`cleared` log lines, 22/22 steps. The free tier's virtual ECU has no fault monitors to
provoke, so this job still cannot pass under `--virtual`; see the campaign file's header
comment.

---

**SAFETY CONTEXT:** Example application — not safety-assessed.
**SPDX-License-Identifier:** Apache-2.0
