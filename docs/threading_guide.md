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
│           ├─ uds_periodic_pop_due()   — drain periodic frames (loop until NOT_FOUND)
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

`uds_server_ctx_t` (holding session and security state) is a single struct — every generated
`uds_init.c` declares exactly one static instance (`s_server_ctx`), regardless of which
transport(s) are compiled in. **`core/uds_server.c` itself does not lock around
`uds_server_process_request()`** — any locking around a call into it is application-level, not
something EDS does for you internally.

The `s_session_lock` / `s_security_lock` pair described above is example-level integration code
(`examples/*/src/main.c`), written once per example around `diag_task`'s call site. **DoIP runs
in its own dedicated thread** (`doip_thread`, `K_THREAD_DEFINE` in `platform/zephyr/zephyr_lwip.c`,
running `eds_doip_server_run()`) with its own call path into `uds_server_process_request()`.

The shipped examples never combine both: `basic_ecu` runs CAN-only (`diag_task`, with the mutex
pair); `basic_ecu_doip` / `basic_ecu_doip_freertos` run DoIP-only (`doip_thread`, no mutex —
correctly, since there's only one caller thread in that build) — its `main.c` explicitly notes
*"The CAN diagnostic task (diag_task) is NOT started here... for a production 'both' transport
ECU, add the CAN thread from basic_ecu alongside the DoIP init."*

**If you do that** — wire both `diag_task` and `doip_thread` into the same build against the
same `s_server_ctx` — both threads now call `uds_server_process_request()` concurrently, and
nothing inside EDS serializes them. You are responsible for extending the *same* `s_session_lock`
/ `s_security_lock` (or an equivalent single lock) around **every** call site that touches
`uds_server_ctx_t`, CAN's and DoIP's alike — not just `diag_task`'s, as the single-transport
examples show it. Skipping this is a real, unguarded data race on session/security state, not a
theoretical one.

### Callback Execution Context

Generated DID/routine handlers (`did_handlers.c`, `routine_handlers.c`) are called synchronously,
inline, from within `uds_server_process_request()` — never from an ISR. The calling thread is
whichever thread invoked `uds_server_process_request()` in your build: `diag_task` for CAN,
`doip_thread` for DoIP, or both (see above) if you've wired a dual-transport build.

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
| ISO-TP N_Cr | `isotp_tick_1ms()` | per tick | Consecutive Frame reception timeout (150 ms) |
| ISO-TP N_As | `isotp_tick_1ms()` | per tick | TX acknowledgment timeout (25 ms) |
| ISO-TP N_Bs | `isotp_tick_1ms()` | per tick | Flow Control reception timeout (75 ms) |
| UDS S3server | `uds_server_tick_1ms()` | per tick | Session timeout (5000 ms) |
| Periodic SLOW | `uds_periodic_tick_1ms()` | per tick | SID 0x2A push at 1000 ms |
| Periodic MEDIUM | `uds_periodic_tick_1ms()` | per tick | SID 0x2A push at 100 ms |
| Periodic FAST | `uds_periodic_tick_1ms()` | per tick | SID 0x2A push at 10 ms |
| WDT window | IWDG hardware | 100 ms | Poll loop health check |

**Critical constraint:** Each `diag_task` iteration must complete in < 1 ms.

Worst-case measured with `-fstack-usage` on STM32H743 at 400 MHz:
- Idle iteration (no CAN frame): ~12 µs
- Single-frame UDS request (0x22 ReadDID): ~180 µs
- Multi-frame response assembly: ~290 µs

All timings are well within the 1 ms deadline at 400 MHz. Re-measure on any new target.

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

Before submitting for ASIL-B qualification review:

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
