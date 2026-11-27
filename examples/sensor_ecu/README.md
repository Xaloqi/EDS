# Sensor ECU Example — Xaloqi EDS

**Zone Controller with Sensor-Driven DTCs — 7 DIDs · 4 DTCs · Real Zephyr Sensor API**

**[EDS#339] Fixed 2026-10:** `native_sim` previously failed to link (no backing driver
for the devicetree stub nodes) and the three live-sensor DIDs plus the two calibration
DIDs returned static mock data disconnected from `sensor_monitor.c`'s real state. Both
are fixed: a minimal native_sim-only driver (`drivers/sensor_stub/`) now backs the
devicetree nodes, and the five DIDs and two routines are `source: app` in
`diagnostics_config.yaml`: their handlers are in `src/diag_app.c`, delegating to
`sensor_monitor.c`'s `sensor_state_get()` / threshold setters instead of a mock array.
Codegen declares and registers them and emits no body, so regenerating leaves them
alone (EDS#613; before that they were hand edits in `generated/`). See
`src/sensor_monitor.c`'s header comment for the driver.

This example demonstrates one real, working pattern end to end — Zephyr sensor API →
100 ms monitoring thread → automatic DTC activation/clear via
`dtc_database_set_status()`, **and** UDS `0x22`/`0x2E` reading/writing that same live
state, not a disconnected mock.

---

## What makes this example different

```
Zephyr sensor API (sensor_sample_fetch / sensor_channel_get)
    │
    ▼
sensor_monitor thread  (100 ms cycle, priority 7)
    │  compares against thresholds
    ├─▶ dtc_database_set_status()    ← DTCs activate/clear automatically, for real
    │
    ▼
sensor_state_get() / sensor_set_temp_threshold_*()
    │
    ▼
src/diag_app.c ('source: app' handlers)  ← UDS 0x22/0x2E/0x31 read/write the same state
```

**DIDs** (`diagnostics_config.yaml`): VIN, ECU serial number (still static mocks —
out of EDS#339's scope, no real VIN/serial source in this example), AmbientTemperature,
SupplyVoltage, SensorStatusBitmask, TemperatureThresholdHigh/Low (all five wired to
real sensor state).
**Routines:** `ResetSensorCalibration` (0xFF00) and `SensorSelfTest` (0xFF01), also
wired to real state.
**DTCs:** ambient-temperature over-range and under-range, supply-voltage over-range
and under-range — all four self-clearing.

The two stub sensor readings on `native_sim` are fixed nominal values (25 degC,
12.0 V) — there's no physical sensor to simulate, and DTC firing/clearing is
demonstrated by writing extreme thresholds via 0xD010/0xD011 (see the
`calibration_write` / `dtc_clear_and_verify` jobs below), not by varying the reading.

---

## What this example contains

```
sensor_ecu/
├── diagnostics_config.yaml      7 DIDs, 4 DTCs — sensor-focused config
├── src/main.c                   Application entry + sensor monitoring thread
├── src/sensor_monitor.c         100 ms thread: sample → check thresholds → set DTCs
├── drivers/sensor_stub/         [EDS#339] native_sim-only fixed-value sensor driver
├── dts/bindings/sensor/         [EDS#339] devicetree bindings for the above
├── CMakeLists.txt
├── prj.conf                     CONFIG_SENSOR=y, CONFIG_CAN=y, CONFIG_WATCHDOG=y
├── boards/native_sim/
└── generated/                   Pre-built codegen output + test suite
```

---

## Building (native_sim)

```
west build -b native_sim examples/sensor_ecu \
  -- -DDIAG_SKIP_CODEGEN=ON \
  -DEXTRA_CONF_FILE=<eds>/examples/sensor_ecu/boards/native_sim/native_sim.conf \
  -DDTC_OVERLAY_FILE=<eds>/examples/sensor_ecu/boards/native_sim/native_sim.overlay
```

The explicit `-DEXTRA_CONF_FILE`/`-DDTC_OVERLAY_FILE` are required — Zephyr does not
auto-discover this nested `boards/native_sim/native_sim.{conf,overlay}` layout (same
gap O-132 found and documented for `examples/bms_ecu`). `-DDIAG_SKIP_CODEGEN=ON` builds
from the committed `generated/` (it's the default here, so the flag is redundant —
included to match CI's own invocation).

### Building for the FRDM-MCXN947

```
west build -b frdm_mcxn947/mcxn947/cpu0 examples/sensor_ecu -- \
  -DDIAG_SKIP_CODEGEN=ON \
  "-DEXTRA_CONF_FILE=<eds>/boards/frdm_mcxn947/frdm_mcxn947.conf" \
  "-DDTC_OVERLAY_FILE=<eds>/boards/frdm_mcxn947/frdm_mcxn947.overlay;<eds>/examples/sensor_ecu/boards/frdm_mcxn947/frdm_mcxn947_sensor.overlay"
```

The second overlay switches off the board's LPCMP comparator node. With it enabled,
Zephyr v3.7.0's `mcux_lpcmp` sensor driver does not compile
([#645](https://github.com/Xaloqi/EDS/issues/645)); this example does not read the
comparator. Compile-checked on Zephyr v3.7.0 and v4.4.1 and built in CI; it has not been
run on the board.

---

## Availability

Free to build, run and study. The example is Apache-2.0; the EDS runtime it links is
GPL v2. Regenerating `generated/` from a changed `diagnostics_config.yaml` needs a
**Developer** licence ([xaloqi.com](https://xaloqi.com)); shipping the runtime in
proprietary firmware needs a commercial licence. See the repository README, "Open-core".

---

See also: [`examples/sensor_ecu_freertos/`](../sensor_ecu_freertos/) — same sensor
pattern on FreeRTOS.  
See also: [`examples/basic_ecu/`](../basic_ecu/) — free community example.
