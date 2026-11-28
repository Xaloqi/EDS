# dual_transport_ecu — CAN and DoIP on one ECU

One UDS server, two transports at once: ISO-TP over CAN (classic or CAN FD) and
DoIP over Ethernet. Both are producers of the diagnostic executor
(`core/uds_exec.h`, `platform/zephyr/zephyr_exec.c`), the one thread that owns
the server. Neither transport calls the server from its own thread.

The DID, DTC and routine set is `basic_ecu_doip`'s (5 DIDs, 2 DTCs, 3 routines).

## Threads

| Thread | Priority | What it does |
|---|---|---|
| `can_task` | 4 | Every 1 ms, `can_producer_step()` (`src/can_producer.c`): drains every queued CAN frame (EDS#558), ticks ISO-TP, posts a reassembled request without waiting for it, sends finished responses and frames the executor routes to it (a job's 7F xx 78 and final answer, 0x2A periodic messages as UUDT frames on 0x5E8). |
| `eds_exec` | 5 | The executor: 1 ms ticks (S3, SecurityAccess lockout, periodic), every dispatch, round-robin over the two producers, the pending job, the ECUReset. Feeds the watchdog after each pass, only while both other threads are alive. |
| `doip_thread` | 7 | `eds_doip_server_run()` with `eds_doip_set_executor()`: posts each request and waits for its answer. |

`can_task` runs above the executor, so CAN reception keeps going while a long
dispatch runs. While its own request is posted, the CAN producer does not touch
that request's buffers: a second physical request is answered `7F xx 21` by the
producer, and a functional one is dropped.

**Sessions.** A session belongs to the transport that opened it: while CAN holds
an extended session, DoIP's physical requests get `7F xx 21`, and the other way
round. A DoIP disconnect ends DoIP's session only. 0x2A over DoIP answers NRC
0x22 (no periodic TX on DoIP yet).

## Build

From a west workspace (Zephyr v3.7), with `X=examples/dual_transport_ecu`:

```sh
# native_sim: CAN on the host SocketCAN interface zcan0, DoIP on the TAP zeth (192.0.2.1)
west build -b native_sim $X -- -DDIAG_SKIP_CODEGEN=ON \
  -DEXTRA_CONF_FILE=$X/boards/native_sim/native_sim.conf \
  -DDTC_OVERLAY_FILE=$X/boards/native_sim/native_sim.overlay

# FRDM-MCXN947: FlexCAN0 + on-board Ethernet (10.10.10.1), MTU 1498 (EDS#648)
west build -b frdm_mcxn947/mcxn947/cpu0 $X -- -DDIAG_SKIP_CODEGEN=ON \
  "-DEXTRA_CONF_FILE=boards/frdm_mcxn947/frdm_mcxn947.conf;$X/boards/frdm_mcxn947/frdm_mcxn947.conf" \
  -DDTC_OVERLAY_FILE=boards/frdm_mcxn947/frdm_mcxn947.overlay

# NUCLEO-H753ZI: FDCAN1 + on-board Ethernet (10.10.10.1)
west build -b nucleo_h753zi $X -- -DDIAG_SKIP_CODEGEN=ON \
  -DEXTRA_CONF_FILE=$X/boards/nucleo_h753zi/nucleo_h753zi.conf \
  -DDTC_OVERLAY_FILE=boards/nucleo_h753zi/nucleo_h753zi.overlay \
  -DEXTRA_DTC_OVERLAY_FILE=$X/boards/nucleo_h753zi/nucleo_h753zi.overlay
```

**CAN FD:** add `$X/fd.conf` to `EXTRA_CONF_FILE` (`CONFIG_CAN_FD_MODE=y`). The
example then opens its ISO-TP channel as an FD channel (TX_DL 64, bit rate switch
on). On the FRDM-MCXN947 also add
`examples/canfd_ecu/boards/frdm_mcxn947/frdm_mcxn947_canfd.overlay` to
`DTC_OVERLAY_FILE` for the data bit rate.

## Run on native_sim

`scripts/smoke_dual_transport_ecu.py` starts the ECU and drives it with a CAN
tester (the Linux kernel's ISO-TP on a vcan) and a DoIP tester (TCP) at once. No
sudo is needed: a user and network namespace holds the vcan and the TAP. The host
needs the `vcan` and `can-isotp` modules.

```sh
unshare -rn sh -c 'ip link add zcan0 type vcan && ip link set zcan0 up &&
  ip tuntap add zeth mode tap && ip link set zeth up && ip addr add 192.0.2.2/24 dev zeth &&
  python3 scripts/smoke_dual_transport_ecu.py build/zephyr/zephyr.exe'
```

For the FD build, give the vcan `mtu 72` and pass `--fd`.

The script checks, in order: session ownership both ways; both testers at once;
a request mix (3E 00, 22 F190, 22 F187, 10 03, 10 01) that competes for the
session, where every physical request is answered or refused `7F sid 21`, a
transport that holds a session is never refused, and the other one never gets
an extended-only answer; DoIP connecting and disconnecting while CAN holds a
session, which must leave CAN's session alone; the p99 round trip of both
testers under load against `P2server_max`; and, after an ECUReset, that the
`request_count` in the ECU's reset log equals the number of requests sent.

## What has run, and where

| What | Where |
|---|---|
| Built | native\_sim (classic and FD), FRDM-MCXN947 (classic and FD), NUCLEO-H753ZI |
| `smoke_dual_transport_ecu.py`, classic and FD | native\_sim, in CI (`example-dual-transport-runtime`) |
| The CAN producer's contracts and C2 (CAN + DoIP producers, ThreadSanitizer) | host, `tests/concurrency/`, in CI (`concurrency-tsan`) |
| Hardware | not run on a board yet |
