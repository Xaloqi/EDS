# Sensor ECU FreeRTOS Example — Xaloqi EDS

**Live Sensor Integration on FreeRTOS — 7 DIDs · 4 DTCs**

This is the FreeRTOS counterpart to [`examples/sensor_ecu/`](../sensor_ecu/).  
It demonstrates the complete pattern — a cyclic simulated sensor, automatic DTC
lifecycle, and live DID responses wired to that same state — running on FreeRTOS
instead of Zephyr.

**[EDS#349] Fixed 2026-10:** DID 0xD001/0xD002/0xD003 reads and 0xD010/0xD011
read+write previously returned static mock data — `src/did_handlers_impl.c` had
the real implementation, but it was wired to a CI step ("Install sensor DID
implementations") that never existed, so no build (including CI) ever used it.
That logic now lives directly in `generated/did_handlers.c`'s `[APP HOOK]`
bodies (same pattern as `examples/sensor_ecu`, EDS#339); `did_handlers_impl.c`
is deleted.

The reference for FreeRTOS teams who want live sensor data exposed over UDS diagnostics.

---

## What this example shows

Same sensor monitoring architecture as `sensor_ecu`, but wired to the FreeRTOS
platform HAL — a software-only cyclic fault pattern (no real sensor API; see
`src/sensor_monitor_freertos.c`'s header comment), not a hardware stub:

```
sensor_monitor task (FreeRTOS, 100 ms, priority 4)
    │
    ├── cycles through normal → over-temp → under-voltage → recovery
    ├── checks thresholds
    ├── dtc_database_set_status()           ← DTC lifecycle fully automatic
    └── sensor_state_get() / sensor_set_temp_threshold_*()
            │
            ▼
    generated/did_handlers.c [APP HOOK] bodies  ← UDS 0x22/0x2E read/write
                                                    that same live state
```

---

## What this example contains

```
sensor_ecu_freertos/
├── diagnostics_config.yaml      Same YAML as sensor_ecu (RTOS-agnostic)
├── src/main.c                   FreeRTOS entry point (main())
├── src/sensor_monitor_freertos.c 100 ms monitoring task (cyclic sim)
├── boards/qemu_cortex_m4/       QEMU Cortex-M4 configuration
└── generated/                   Pre-committed codegen output — NOT regenerated
                                  by CI (CI only verifies these files exist;
                                  see .github/workflows/ci.yml's
                                  freertos-examples job)
```

Board support — reset vector, C runtime startup, FreeRTOS static-allocation
hooks and the boot log — comes from the shared
`examples/common/boards/qemu_cortex_m4/`, not from this directory.

**Run it:**

```sh
qemu-system-arm -machine mps2-an386 -cpu cortex-m4 \
  -kernel build/eds_sensor_freertos.elf \
  -nographic -monitor none -serial file:boot.log
```

`boot.log` should contain:

```
EDS sensor_ecu_freertos: boot
EDS sensor_ecu_freertos: starting scheduler
```

Nothing at all means the image is not running — see
`examples/basic_ecu_freertos/README.md` for how to tell an empty binary from a
working one (EDS issue #268).

**Platform:** Builds for QEMU ARM Cortex-M4 in CI.  
**Note:** `generated/` is pre-committed, same as every other example — CI does
not regenerate it, only verifies the files are present. If you edit
`diagnostics_config.yaml`, regenerate locally and reapply the `[APP HOOK]`
bodies in `generated/did_handlers.c`/`generated/routine_handlers.c` (EDS#349)
before committing.

---

## Availability

Included with **Developer** and **Professional** licenses.

**[Purchase at xaloqi.com →](https://xaloqi.com)**

---

See also: [`examples/sensor_ecu/`](../sensor_ecu/) — same example on Zephyr.  
See also: [`examples/basic_ecu_freertos/`](../basic_ecu_freertos/) — free community FreeRTOS example.
