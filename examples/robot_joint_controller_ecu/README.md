# Robot Joint Controller Example — Xaloqi EDS

**UDS diagnostics for a robot joint — 10 DIDs · 5 DTCs · 3 routines · Zephyr, `native_sim`**

This example is a **robot joint controller ECU**: one servo axis exposing position,
velocity, torque, motor temperature and a status bitmask over ISO 14229 UDS on CAN, with
an over-temperature DTC and a writable calibration block. It runs on `native_sim` against a
**simulated joint** and you can query it from the host with any UDS tester.

**Why UDS for robotics?** If your robot uses CAN, you already have the hardware. UDS gives
you live joint telemetry readable by standard tools, a fault list, and calibration
write-back that needs an extended session and security access — all from one
`diagnostics_config.yaml`.

**What is verified, and what is not:** this has only run on `native_sim` (see
[Test boundary](#test-boundary)). The joint is a model in `src/joint_sim.c`; no physical
joint, motor or encoder has been validated.

---

## What this example contains

```
robot_joint_controller_ecu/
├── diagnostics_config.yaml      10 DIDs, 5 DTCs, 3 routines
├── src/main.c                   application entry point
├── src/joint_sim.c              simulated joint behind DIDs 0xA000-0xA004
├── boards/native_sim/           loopback and host-reachable (SocketCAN) variants
├── CMakeLists.txt
└── generated/                   committed codegen output (nothing edited by hand)
```

**DIDs (10)**

| DID | Name | Encoding | Source |
|---|---|---|---|
| 0xF190 | RobotSerialNumber | 17 bytes ASCII | generated (fixed value) |
| 0xF18C | ControllerSerialNumber | 8 bytes ASCII | generated (fixed value) |
| 0xA000 | Axis0Position | int32 BE, 0.001 ° | **live**, `joint_sim.c` |
| 0xA001 | Axis0Velocity | int16 BE, 0.01 rpm | **live**, `joint_sim.c` |
| 0xA002 | Axis0Torque | int16 BE, 0.01 Nm | **live**, `joint_sim.c` |
| 0xA003 | Axis0MotorTemperature | uint8, raw = °C + 40 | **live**, `joint_sim.c` |
| 0xA004 | Axis0StatusBitmask | uint8 bitmask | **live**, `joint_sim.c` |
| 0xA010 | Axis0PositionOffset | int32 BE, mdeg, read/write | generated store, extended session + security level 1 to write |
| 0xA011 | Axis0SoftLimitPositive | int32 BE, mdeg, read/write | generated store, +170 000 mdeg at boot |
| 0xA012 | Axis0SoftLimitNegative | int32 BE, mdeg, read/write | generated store, −170 000 mdeg at boot |

**DTCs (5):** 0xA00101 motor over-temperature, 0xA00201 drive over-current, 0xA00103
encoder signal loss, 0xA00301 positive soft limit, 0xA00302 negative soft limit.
Only 0xA00101 is driven by the simulation.

**Routines (3):** `HomeAxis0` (0xFF00), `ApplyCalibration` (0xFF01),
`ClearFaultHistory` (0xFF02). These are the generated stubs; extended session and security
level 1 are required.

### The simulated joint

The five live DIDs are `source: app` in the YAML: codegen emits no body for them and
`src/joint_sim.c` defines `did_read_<name>()`. To use real hardware, replace that file.
Everything is a pure function of the time since boot (integer maths, no heap, no
recursion), so a run is reproducible.

| Signal | Model |
|---|---|
| Position | triangle wave between −90° and +90°, 20 s per cycle (18 °/s), inside the ±170° soft limits |
| Velocity | ±3.00 rpm, the slope of the position |
| Torque | ±0.40 Nm friction in the direction of travel plus 0.01 Nm per 0.5° of position |
| Motor temperature | 30 °C at boot, +1 °C per second (accelerated for the demo), saturating at 100 °C |
| Status 0xA004 | `0x09` (ENABLED, HOME_COMPLETE); `0x1D` once over temperature (adds FAULT_ACTIVE bit 2, OVERTEMP bit 4) |
| DTC 0xA00101 | set when the temperature exceeds 85 °C, i.e. at 86 °C, **56 s after boot**; status byte `0x2B` (testFailed, thisOperationCycle, confirmed, failedSinceLastClear) |

The DTC is set with `dtc_database_set_status()`, the call `sensor_ecu` uses. The
simulation does not cool down, so the DTC stays set; clearing it (0x14, or routine 0xFF02)
works until the next 100 ms re-assertion, since the temperature is still above 85 °C.

---

## Building

Two `native_sim` variants. Both pass `-DDIAG_SKIP_CODEGEN=ON` (the committed `generated/`
is used, so no licence or `pyyaml`/`jinja2` is needed).

**Host-reachable (SocketCAN) — use this to query it from the host:**

```bash
EX=<eds>/examples/robot_joint_controller_ecu
west build -b native_sim $EX \
  -- -DDIAG_SKIP_CODEGEN=ON \
     -DEXTRA_CONF_FILE=$EX/boards/native_sim/native_sim_socketcan.conf \
     -DDTC_OVERLAY_FILE=$EX/boards/native_sim/native_sim_socketcan.overlay
```

`can0` is then Zephyr's `zephyr,native-linux-can` node, bound to the host CAN interface
**`zcan0`**.

**Loopback — compile check only:** the same command with `native_sim.conf` and
`native_sim.overlay`. `can_loopback0` talks only to itself, so no tester can reach it.
CI builds this one in the Zephyr examples matrix.

## Run it and query it from the host

You need a CAN interface named `zcan0` (a virtual one is enough) and the kernel's
`vcan` and `can_raw` modules. Without root, a user + network namespace gives you one;
run the ECU and the tester **inside the same namespace**:

```bash
unshare -rn bash -c '
  ip link add zcan0 type vcan && ip link set zcan0 up
  ./build/zephyr/zephyr.exe &
  sleep 1
  stdbuf -oL isotprecv -l -s 7DF -d 7E8 zcan0 &
  sleep 0.3
  echo "22 A0 00" | isotpsend -s 7DF -d 7E8 zcan0    # position
  echo "22 A0 03" | isotpsend -s 7DF -d 7E8 zcan0    # motor temperature
  sleep 1; kill %1 %2'
```

With root, create the interface once (`ip link add zcan0 type vcan`) or give an existing
one the alternate name: `ip link property add dev vcan0 altname zcan0`.

Addressing is 0x7DF (request) → 0x7E8 (response). Any
ISO-TP/UDS tool on SocketCAN works; the free `xaloqi-tester` package's SocketCAN transport
is a Pro plugin, so the examples here use `can-utils` and Python's kernel ISO-TP sockets.

Things to try, in order:

| Request | What you see |
|---|---|
| `22 A0 00` | `62 A0 00` + position, e.g. `FF FE FC 44` = −66.5 ° |
| `22 A0 03` | `62 A0 03` + `47` = 31 °C at the start, rising 1 °C/s |
| `19 02 01` | at boot `59 02 FF` (no DTC with testFailed set); after 56 s `59 02 FF A0 01 01 2B` |
| `2E A0 10 …` in the default session | negative response `7F 2E 7F` |
| `10 03`, then `2E A0 10 00 00 00 64` | `7F 2E 33`, security access denied |
| `27 01`, `27 02 <key>`, then the `2E` again | `6E A0 10`; `22 A0 10` reads back `00 00 00 64` |

The key for `27 02` is the AES-128-CMAC of the seed under the **placeholder** key every
example ships with (`xaloqi.tester` derives it); the ECU logs
"Placeholder AES keys active". Production firmware must inject real keys.

`scripts/smoke_robot_joint_ecu.py` automates the first three rows; CI runs it
(`example-robot-runtime`):

```bash
unshare -rn bash -c 'ip link add zcan0 type vcan && ip link set zcan0 up &&
  python3 scripts/smoke_robot_joint_ecu.py build/zephyr/zephyr.exe zcan0'
```

## Regenerating

`generated/` is committed. If you change `diagnostics_config.yaml`, regenerate with the
Developer licence:

```bash
python3 tools/codegen.py --config examples/robot_joint_controller_ecu/diagnostics_config.yaml \
                         --out    examples/robot_joint_controller_ecu/generated/ \
                         --safety-wrappers --asil-level B --no-manifest
```

`src/joint_sim.c` is not touched by codegen (the five live DIDs are `source: app`).

---

## Test boundary

- **Verified:** builds on `native_sim` (loopback and SocketCAN variants); over `vcan`
  the live DIDs return the simulated values above, the over-temperature DTC appears in
  `0x19` after the ramp, and `0x2E` to 0xA010 is refused until an extended session and
  security access. CI compiles the loopback variant and runs the runtime smoke on the
  SocketCAN variant.
- **Not verified:** any hardware. No physical joint, motor, encoder or temperature sensor
  has been connected, and no MCU board has run this example. Position, velocity, torque
  and temperature are a model, not measurements.
- Routines 0xFF00–0xFF02 and the soft-limit DTCs are generated stubs and are not exercised
  by the smoke.

---

## Availability

Free to build, run and study. The example is Apache-2.0; the EDS runtime it links is
GPL v2. Regenerating `generated/` from a changed `diagnostics_config.yaml` needs a
**Developer** licence ([xaloqi.com](https://xaloqi.com)); shipping the runtime in
proprietary firmware needs a commercial licence. See the repository README, "Open-core".

---

See also: [`examples/sensor_ecu/`](../sensor_ecu/) — live sensor pattern with a monitor thread.  
See also: [`examples/basic_ecu/`](../basic_ecu/) — the smallest example.
