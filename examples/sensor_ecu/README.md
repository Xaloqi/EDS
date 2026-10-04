# Sensor ECU Example — Xaloqi EDS

**Zone Controller with Sensor-Driven DTCs — 7 DIDs · 4 DTCs · Real Zephyr Sensor API**

**[EDS#339] Corrected 2026-10:** this README previously claimed DID reads return live
sensor values and listed a DID set (humidity, pressure, 3-axis accelerometer, etc.) that
doesn't match `diagnostics_config.yaml`. Neither was accurate. What's actually true,
below — and `native_sim` currently fails to link for an unrelated reason (missing stub
sensor driver); see `src/sensor_monitor.c`'s header comment.

This example demonstrates one real, working pattern — Zephyr sensor API → 100 ms
monitoring thread → automatic DTC activation/clear via `dtc_database_set_status()` — on
top of the same static-mock DID read handlers every other example uses. The sensor
thread's readings and what UDS `0x22` returns are two separate things here, not one
pipeline; see `sensor_monitor.c` and `diagnostics_config.yaml`'s header comments for the
exact boundary.

---

## What makes this example different

Every example (including this one) returns static stub values from its DID read
handlers (`generated/did_handlers.c`). What's different here is the DTC half:

```
Zephyr sensor API (sensor_sample_fetch / sensor_channel_get)
    │
    ▼
sensor_monitor thread  (100 ms cycle, priority 7)
    │  compares against thresholds
    ▼
dtc_database_set_status()    ← DTCs activate/clear automatically, for real
```

**DIDs** (`diagnostics_config.yaml`): VIN, ECU serial number, AmbientTemperature,
SupplyVoltage, SensorStatusBitmask, TemperatureThresholdHigh/Low.
**DTCs:** ambient-temperature over-range and under-range, supply-voltage over-range
and under-range — all four self-clearing.

---

## What this example contains

```
sensor_ecu/
├── diagnostics_config.yaml      7 DIDs, 4 DTCs — sensor-focused config
├── src/main.c                   Application entry + sensor monitoring thread
├── src/sensor_monitor.c         100 ms thread: sample → check thresholds → set DTCs
├── CMakeLists.txt
├── prj.conf                     Adds CONFIG_SENSOR=y, CONFIG_BME280=y, CONFIG_LIS2DH=y
├── boards/native_sim/
└── generated/                   Pre-built codegen output + test suite
```

---

## Availability

Included with **Developer** and **Professional** licenses.

**[Purchase at xaloqi.com →](https://xaloqi.com)**

---

See also: [`examples/sensor_ecu_freertos/`](../sensor_ecu_freertos/) — same sensor
pattern on FreeRTOS.  
See also: [`examples/basic_ecu/`](../basic_ecu/) — free community example.
