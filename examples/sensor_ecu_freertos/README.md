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
That logic now lives in `src/diag_app.c`: the five DIDs and two routines are
`source: app` in `diagnostics_config.yaml`, so codegen declares and registers
them and emits no body (EDS#613, same as `examples/sensor_ecu`);
`did_handlers_impl.c` is deleted.

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
    src/diag_app.c ('source: app' handlers)  ← UDS 0x22/0x2E/0x31 read/write
                                                that same live state
```

---

## What this example contains

```
sensor_ecu_freertos/
├── diagnostics_config.yaml      Same YAML as sensor_ecu (RTOS-agnostic)
├── src/main.c                   FreeRTOS entry point (main())
├── src/sensor_monitor_freertos.c 100 ms monitoring task (cyclic sim)
├── src/diag_app.c               The 'source: app' DID and routine handlers
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
`diagnostics_config.yaml`, regenerate locally before committing; the handlers
in `src/diag_app.c` are not touched by codegen.

---

## Availability

Free to build, run and study. The example is Apache-2.0; the EDS runtime it links is
GPL v2. Regenerating `generated/` from a changed `diagnostics_config.yaml` needs a
**Developer** licence ([xaloqi.com](https://xaloqi.com)); shipping the runtime in
proprietary firmware needs a commercial licence. See the repository README, "Open-core".

---

See also: [`examples/sensor_ecu/`](../sensor_ecu/) — same example on Zephyr.  
See also: [`examples/basic_ecu_freertos/`](../basic_ecu_freertos/) — free community FreeRTOS example.
