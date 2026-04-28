# Sensor ECU Example — Xaloqi EDS

**Zone Controller with Live Sensor Integration — 7 DIDs · 4 DTCs · Real Zephyr Sensor API**

This is the **only Xaloqi EDS example that reads real sensor data** rather than returning
placeholder values. It demonstrates the complete production pattern: Zephyr sensor API →
100 ms monitoring thread → automatic DTC activation/clear → live DID responses.

The reference for any ECU that exposes physical sensor readings over UDS.

---

## What makes this example different

Every other example returns static stub values from DID handlers. This example shows:

```
Zephyr sensor API (sensor_sample_fetch / sensor_channel_get)
    │
    ▼
sensor_monitor thread  (100 ms cycle, priority 7)
    │  compares against thresholds
    ▼
dtc_database_set_status()    ← DTCs activate/clear automatically
    │
    ▼
DID read handler             ← returns live sensor value
    │
    ▼
UDS ReadDataByIdentifier (0x22) response
```

**DIDs:** Ambient temperature, humidity, pressure, accelerometer (3-axis), battery voltage, supply rail  
**DTCs:** OverTemperature, UnderVoltage, SensorCommFault, AccelerometerFault — all self-clearing

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
