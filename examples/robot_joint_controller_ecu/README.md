# Robot Joint Controller Example — Xaloqi EDS

**UDS Diagnostics for Robotics — 8 DIDs · 4 DTCs · 3 routines · Zephyr RTOS**

This example demonstrates Xaloqi EDS on a **robot joint controller ECU** — a
collaborative robot arm joint managing position, velocity, torque, and thermal state
over CAN with the same ISO 14229 UDS protocol used in automotive ECUs.

**Why UDS for robotics?** If your robot uses CAN, you already have the hardware.
UDS gives you live joint telemetry readable by any standard scan tool, persistent
fault history that survives power cycles, calibration write-back in extended session,
and the same testing infrastructure used by automotive Tier-1 suppliers — all from
a single `diagnostics_config.yaml`.

---

## What this example contains

```
robot_joint_controller_ecu/
├── diagnostics_config.yaml      8 DIDs, 4 DTCs, 3 routines
├── src/main.c
├── CMakeLists.txt
└── generated/                   Pre-built codegen output
```

**DIDs:** Joint position, velocity, torque, winding temperature, encoder count,
control mode, firmware version, error register  
**Routines:** Encoder calibration, home position sweep, thermal runaway test  
**DTCs:** OverTemperature, EncoderFault, TorqueLimitExceeded, PositionError

---

## Availability

Included with **Developer** and **Professional** licenses.

**[Purchase at xaloqi.com →](https://xaloqi.com)**

---

See also: [`examples/sensor_ecu/`](../sensor_ecu/) — live sensor pattern.  
See also: [`examples/basic_ecu/`](../basic_ecu/) — free community example.
