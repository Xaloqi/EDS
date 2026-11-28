# Threading & RTOS Integration Guide
## Xaloqi EDS — Phase 6A

---

## Overview

Phase 6A transitions the diagnostics stack from a bare poll loop (`while(true) + k_msleep`) to a
first-class Zephyr thread architecture with hardware watchdog supervision, mutex-protected shared
state, and a k_timer-driven 1 ms tick. This document describes the thread model, synchronisation
design, timing constraints, and production checklist.

---

## Thread Model

```
Zephyr Kernel
│
├── main thread (exits after init)
│     ├─ diag_mutex_init()       — session + security k_mutex objects
│     ├─ diag_wdt_init()         — IWDG channel (100 ms window)
│     ├─ diag_timer_init()       — 1 ms k_timer + k_sem
│     ├─ zephyr_port_init()      — CAN controller + RX filter
│     ├─ uds_generated_init()    — full UDS/ISO-TP stack
│     └─ k_thread_create()  ─────────────────────────────────────────────┐
│                                                                          │
├── diag_task  (priority 5, stack 4096 bytes)  <─────────────────────────┘
│     └── loop every 1 ms:
│           ├─ diag_timer_wait_tick()   — block on k_sem from k_timer ISR
│           ├─ can_transport_receive()  — poll k_msgq (from CAN RX ISR)
│           ├─ isotp_process_rx_frame() — reassemble ISO-TP PDU
│           │    └─ on_isotp_rx_complete()
│           │         ├─ diag_mutex_lock(session + security)
│           │         ├─ uds_server_process_request()
│           │         ├─ diag_mutex_unlock(session + security)
│           │         └─ isotp_transmit()
│           ├─ isotp_tick_1ms()         — N_Cr / N_As / N_Bs timers
│           ├─ uds_server_tick_1ms()    — S3server + lockout timers
│           ├─ uds_periodic_tick_1ms()  — SID 0x2A periodic push timers
│           ├─ uds_periodic_pop_due()   — drain periodic messages (loop until NOT_FOUND), send each with isotp_transmit_uudt()
│           └─ diag_wdt_feed()          — kick IWDG
│
├── CAN ISR (priority 0, triggered by FDCAN1)
│     └─ state_change_cb()   — writes s_bus_off flag (irq_lock protected)
│     └─ RX filter callback  — k_msgq_put() to s_can_rx_msgq
│
└── k_timer ISR (priority 0, 1 ms period)
      └─ timer_expiry_fn()   — k_sem_give() to s_tick_timer.sem
```

---

## Synchronisation Design

### Mutex Usage

Two `diag_mutex_t` objects (`s_session_lock`, `s_security_lock`) protect the UDS session and
security contexts from concurrent modification. In the single-thread Phase 6A design these locks
are always acquired and released within the same `diag_task` iteration — they are designed for
**future expansion** where a second thread (e.g. a background DTC setter or NVM writer) may write
the session type.

**Lock ordering rule** (must be respected by any future thread):
```
Always acquire: s_session_lock  THEN  s_security_lock
Never acquire in reverse order — this prevents priority inversion deadlock.
```

### Bus-Off Flag

`s_bus_off` is a `volatile bool` written from the CAN state-change callback (ISR context) and
read from `diag_task` (thread context). It is protected by `irq_lock()/irq_unlock()` on every
access to guarantee atomic read-modify-write on all architectures.

### CAN RX Queue

`s_can_rx_msgq` is a Zephyr `K_MSGQ_DEFINE` ring buffer, inherently ISR-safe. The CAN driver
calls `k_msgq_put()` from ISR context; `diag_task` calls `k_msgq_get(K_NO_WAIT)` from thread
context. No additional locking is required.

### Dual-Transport Concurrency (CAN + DoIP)

`uds_server_ctx_t` (session, security, pending job, counters) is one struct: every generated
`uds_init.c` declares exactly one static instance, whichever transports are compiled in.
`core/uds_server.c` does not lock around `uds_server_process_request()`, so two threads that call
it, or a thread that calls it while another runs `uds_server_tick_1ms()`, race on that state. The
S3 timer, the SecurityAccess lockout and the periodic scheduler need a 1 ms tick as well, and the
tick is a third caller.

**The one way to run more than one caller is the diagnostic executor (EDS#197).** One thread owns
the server. Each transport is a *producer*: it hands the executor a request and takes the answer
back, and no producer calls the server itself.

```
  CAN thread (src 0)  ──post──►┐
                               ├─►  executor thread  ─►  uds_server_*  (the only caller)
  DoIP thread (src 1) ──post──►┘     1 ms ticks · S3 · lockout · periodic · pending job · ECUReset
```

The executor core is `core/uds_exec.h` (RTOS-free); the bindings are `platform/zephyr/zephyr_exec.h`
and `platform/freertos/freertos_exec.h`. Each producer has one slot (a request in flight, nothing
copied), and the executor serves the slots round-robin, one request per slot per pass.

**Reference: `examples/dual_transport_ecu`** (CAN classic or FD, and DoIP, on Zephyr). Read its
`src/main.c` for the wiring and `src/can_producer.c` for a producer that never blocks. CAN-only
builds (`basic_ecu` and the other CAN examples) have one thread already serialized and need no
executor. DoIP-only builds (`basic_ecu_doip`, `basic_ecu_doip_freertos`) use it for the tick and
the DoIP thread.

#### Wiring

1. Start the executor once, after `uds_generated_init()` and before any producer runs:
   `zephyr_exec_start()` or `freertos_exec_start()`, with the server, one
   `uds_exec_producer_cfg_t` per transport (`src`, and `periodic_tx` if the transport can send a
   periodic message), an optional `reset` function (what an ECUReset does: flush NVM, let the
   response leave, reset) and an optional `after_pass` function, which runs on the executor after
   every pass: feed the watchdog there, only while every producer has checked in.
2. DoIP: `eds_doip_set_executor(zephyr_exec_get(), zephyr_exec_wait_event)`, or
   `freertos_exec_get()` and `freertos_exec_wait_event`, before `eds_doip_platform_start()`. The
   DoIP source is `EDS_DOIP_UDS_SRC` (default 1).
3. CAN: a producer calls `uds_exec_submit()` for each reassembled request. It must not wait for the
   executor: keep draining RX (EDS#558) and ticking ISO-TP, and take the response with
   `uds_exec_done()` when the slot completes. `can_producer_step()` in the example does that.
4. The ECUReset, the periodic message and the job frames come back to the producer
   (`uds_exec_push_frame()`), so a producer sends them on its own transport.

#### Producer contracts

- **`uds_exec_release()` on every exit path** once a request is posted: the response sent, a send
  error, a disconnect, a timeout. A request released before it ran is withdrawn; one released while it
  runs is freed when the executor is done. An ECUReset the request owes is performed after the
  release.
- **A posted slot's buffers belong to the executor** until `uds_exec_done()`, or, after a release,
  until `uds_exec_submit()` returns `POSTED` again. Receive the next request into a buffer of your
  own.
- **A refused request is the producer's to answer.** `UDS_EXEC_REJECT_BUSY` means send
  `7F sid 21` (while its own slot is in use, or while another producer's job step is running, so the
  answer comes within P2); `UDS_EXEC_DROPPED` means send nothing (a functional request).

#### What changes for a tester

- **A session belongs to the transport that opened it.** The transport whose request leaves the
  default session owns it until its own `10 01`, S3, or its disconnect. While it is open, the
  other transport's physical requests get NRC 0x21 and its functional requests no response; they
  change no state and do not restart S3. A DoIP disconnect ends the session only if DoIP owns it. A
  session the application opened itself with `uds_session_transition()` has no owner and serves both.
- **A job another transport started keeps running** when DoIP disconnects (EDS#664); a job DoIP
  started is aborted by its disconnect.
- **Periodic messages (0x2A) go to the transport that subscribed.** DoIP has no periodic TX yet, so
  0x2A over DoIP answers NRC 0x22 (EDS#546).
- **The DoIP thread waits at most `DOIP_EXEC_WAIT_MS` (10 s) for the executor**, then releases the
  request and closes the connection.

#### Costs

RAM, against a DoIP build with the tick task and the lock pair, measured on the `basic_ecu_doip`
builds: the executor adds its response mailbox, one 4 KB message buffer (+4.3 KB on Zephyr, +5.2 KB
static on FreeRTOS), and its stack is 4096 bytes on both RTOSes (`EDS_EXEC_STACK_SIZE`), 3 KB more
than the 1024 bytes it had before DoIP moved onto it, because every dispatch now runs on it. The
CHANGELOG has the per-board figures. Dispatch latency is one executor pass; with a producer
whose job step is running, the other producer's answer is `7F sid 21` within P2.

#### Moving off the lock callbacks

`eds_doip_set_lock_callbacks()` (EDS#353) is **deprecated**: a call warns at build time, and the
next minor release removes it. During this release a production build
(`EDS_BUILD_IS_PRODUCTION`) accepts either an executor or both callbacks, and
`eds_doip_server_run()` refuses to run (`UDS_STATUS_ERR_NOT_INITIALIZED`) when it has neither. With
the callbacks, DoIP calls the server on its own thread between your lock and unlock, and you
remain responsible for taking the *same* lock around every other call that touches the server, the
tick task's included. Migration for a DoIP-only build: drop the tick task and the lock pair, start
the executor, and register it (`basic_ecu_doip` shows the result). The interim `zephyr_exec_lend()`
and `freertos_exec_lend()` helpers of the pre-release work are gone and were never in a release.

#### What is verified

The host ThreadSanitizer tests in `tests/concurrency/` run the example's two producers against the
executor with no data race and no lost update (C2, C3), and show the earlier wiring racing (C2 with
`-DC2_J1_WIRING=1`). `scripts/smoke_dual_transport_ecu.py` loads both transports of the native_sim
build with a request mix that competes for the session, in CI (classic CAN and CAN FD). Nothing
here has run on a board yet; the bench plan is in the knowledge base.

**Known limits.** An ECUReset whose response failed to send is still performed on the executor path
(the deprecated lock path skips it, EDS#434). Codegen emits no executor wiring for any transport
(EDS#357): the application's `main.c` starts it, as the examples do. One executor per image.

### Callback Execution Context

Generated DID/routine handlers (`did_handlers.c`, `routine_handlers.c`) are called synchronously,
inline, from within `uds_server_process_request()` — never from an ISR. The calling thread is
whichever thread invoked `uds_server_process_request()` in your build: `diag_task` for a CAN-only
build, the executor thread for every build that uses the diagnostic executor (see above).

EDS holds no lock while invoking your callback and expects none — synchronizing your callback's
access to shared application state (a sensor reading, a calibration value, anything not owned
exclusively by the calling thread) is entirely your responsibility, using the same primitives
(`diag_mutex_t`, atomics, message queues) you'd use between any two threads touching shared state.
A DID handler reading `app_global_vin` without synchronization races exactly like any other
unsynchronized cross-thread read — nothing UDS-specific makes it safer.

---

## Timing Constraints

| Timer | Source | Period | Purpose |
|---|---|---|---|
| `s_tick_timer` | `k_timer` | 1 ms | Drives ISO-TP + UDS state machines |
| ISO-TP N_Cr | `isotp_tick_1ms()` | per tick | Consecutive Frame reception timeout (1000 ms, ISO 15765-2:2024 Table 22) |
| ISO-TP N_As | `isotp_tick_1ms()` | per tick | TX acknowledgment timeout (25 ms) |
| ISO-TP N_Bs | `isotp_tick_1ms()` | per tick | Flow Control reception timeout (1000 ms, ISO 15765-2:2024 Table 22) |
| UDS S3server | `uds_server_tick_1ms()` | per tick | Session timeout (5000 ms) |
| Periodic SLOW | `uds_periodic_tick_1ms()` | per tick | SID 0x2A push at 1000 ms |
| Periodic MEDIUM | `uds_periodic_tick_1ms()` | per tick | SID 0x2A push at 100 ms |
| Periodic FAST | `uds_periodic_tick_1ms()` | per tick | SID 0x2A push at 10 ms |
| WDT window | IWDG hardware | 100 ms | Poll loop health check |
| Response-pending keepalive | `uds_server_tick_1ms()` + step credit | P2\*/2 (2500 ms) | `7F sid 78` while a deferred job runs (#282) |
| Response-pending cap | `uds_server_tick_1ms()` + step credit | 120 s | Job aborted, `7F sid 10` |

**Critical constraint:** Each `diag_task` iteration must complete in < 1 ms.

Worst-case measured with `-fstack-usage` on STM32H743 at 400 MHz:
- Idle iteration (no CAN frame): ~12 µs
- Single-frame UDS request (0x22 ReadDID): ~180 µs
- Multi-frame response assembly: ~290 µs

All timings are well within the 1 ms deadline at 400 MHz. Re-measure on any new target.

**Response-pending steps are the one exception (#282).** While a deferred job runs (a chunked
0x34 erase, or a routine opted in with `routine_database_set_deferral()`), `uds_server_poll()`
runs one step per iteration, and that step may take up to the job's declared `step_max_ms`
(4000 ms for an STM32H7 sector erase). Rules:

- Call `uds_server_poll()` and `uds_server_frame_sent()` on the thread that owns the server,
  under the same lock as `uds_server_process_request()` and `uds_server_tick_1ms()`. All UDS
  state changes happen inside the step, on that thread. A step whose work is asynchronous (DMA,
  a crypto peripheral) only checks its completion flag.
- A step must not call back into `uds_server_poll()` or `uds_server_process_request()`: the server
  fails the job with `7F sid 10`.
- The 1 ms tick does not run during a step. The server credits the step's `step_max_ms` after it,
  so the keepalive is not delayed. Keep the watchdog fed across a long step as
  `zephyr_flash_ops_set_wdt()` does for the erase.
- Over DoIP on the executor, the job's steps run on the executor thread and `doip_server.c` sends its
  frames; a request from another producer gets `7F sid 21` at once instead of waiting for the step. With
  the deprecated `eds_doip_set_lock_callbacks()` pair, `doip_server.c` polls the job inside the pair and
  sleeps 1 ms between polls (the `sleep_ms` platform op), so the UDS tick task still gets the lock.

---

## Stack Budget

```
diag_task stack:  CONFIG_DIAG_TASK_STACK_SIZE = 4096 bytes

Call depth breakdown (measured with -fstack-usage):
  diag_task_entry()                     48 bytes
  └─ on_isotp_rx_complete()            128 bytes
     └─ uds_server_process_request()   192 bytes
        └─ service_0x22_handler()      320 bytes
           └─ uds_safety_find_did()     96 bytes
              └─ uds_safety_check_*()   64 bytes

Total worst-case depth:  ~848 bytes
Safety margin:           4096 - 848 = 3248 bytes (3.8x)
```

With `CONFIG_STACK_CANARIES=y`, Zephyr detects overflow and triggers a fatal error before
silent corruption occurs.

---

## Watchdog Configuration

| Parameter | Value | Rationale |
|---|---|---|
| `CONFIG_DIAG_WDT_WINDOW_MS` | 100 ms | 100 iterations at 1 ms/loop |
| `WDT_OPT_PAUSE_HALTED_BY_DBG` | enabled | Allows debugger halt without reset |
| `WDT_FLAG_RESET_SOC` | set | Full SoC reset on expiry |

On `native_sim`, the WDT device is absent. `zephyr_wdt.c` detects this via
`DEVICE_DT_GET_OR_NULL(DT_NODELABEL(wdt0))` and degrades gracefully — `diag_wdt_feed()` becomes a
no-op. A `LOG_WRN` is emitted at startup to make this explicit.

---

## Build Instructions

### native_sim (host simulation)

```bash
# One-time vcan setup (Linux):
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0

# Build:
west build -b native_sim examples/basic_ecu \
  -- -DCONF_FILE="prj.conf;../../boards/native_sim/native_sim.conf"

# Run:
./build/zephyr/zephyr.exe &

# Test:
python3 tools/sim_tester.py
```

### nucleo_h743zi (STM32H743 hardware)

```bash
# Build:
west build -b nucleo_h743zi examples/basic_ecu \
  -- -DCONF_FILE="prj.conf;../../boards/nucleo_h743zi/nucleo_h743zi.conf"

# Flash via ST-Link:
west flash

# Monitor UART (115200 baud, ST-Link virtual COM):
minicom -D /dev/ttyACM0 -b 115200
```

---

## Production Checklist

Before submitting your integration for safety assessment:

- [ ] Replace stub security algorithm (`app_security_seed_generate` / `app_security_key_validate`)
      with TRNG-backed OEM-approved key derivation
- [ ] Validate worst-case poll loop time on target MCU at rated clock speed
- [ ] Verify `CONFIG_DIAG_TASK_STACK_SIZE` against `-fstack-usage` output for the target toolchain
- [ ] Enable `CONFIG_STACK_SENTINEL=y` for runtime overflow detection in field firmware
- [ ] Set `CONFIG_ASSERT=n` for final release build if assertion overhead is unacceptable
- [ ] Verify `CONFIG_DIAG_WDT_WINDOW_MS` >= 2× worst-case poll loop time
- [ ] Implement CAN bus-off recovery (see `TODO [APPLICATION]` in `diag_task_entry()`)
- [ ] Qualify FDCAN1 pinout and sample point against target network analyser trace
- [ ] Verify 120 Ω termination resistors at both ends of the CAN bus
- [ ] Document and sign off on MISRA-15.5 deviation record for guard-clause returns
- [ ] Run `west twister -T tests/` on both `native_sim` and `nucleo_h743zi` targets

---

*Document version: Phase 6A · Generated: 2026-03-07*
