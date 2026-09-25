# Performance & WCET Analysis

## Xaloqi EDS — Cortex-M7 Hardware Measurements (Issue #31)

Worst-case execution time (WCET) figures for the three paths issue #31 scoped,
measured on real Cortex-M7 hardware — **not** the x86-64 `native_sim` timing
data referenced elsewhere in this repo, which characterises host-simulator
behaviour only and is not representative of target hardware.

## 1. Scope, board, and why the board changed mid-issue

- **Target**: NUCLEO-H753ZI (STM32H753ZI, Cortex-M7 @ 480 MHz, `-Os`).
  Issue #31 originally named the NUCLEO-H743ZI2; that board was superseded by
  a customer-requested substitution (issue #277) before hardware bring-up —
  same STM32H7 die family, same core, same clock ceiling, so the figures
  below are the intended measurement regardless of which H7 Nucleo variant
  is on the bench.
- **Build**: `west build ... -- -DEXTRA_CFLAGS=-DDIAG_WCET_MEASURE=1`, plus a
  per-board `wcet_measurement.conf` (`CONFIG_TIMING_FUNCTIONS=y`, and
  `CONFIG_INIT_STACKS=y` for the stack cross-check in §4). `DIAG_WCET_MEASURE`
  is never defined by any example's own `CMakeLists.txt`/`prj.conf` — every
  instrumented call site (`platform/zephyr/zephyr_wcet.h`,
  `examples/safeboot_ecu/src/main.c`, `examples/basic_ecu_doip/src/main.c`,
  `transport/doip/doip_server.c`) compiles to nothing without it. Production
  and every existing CI build are unaffected.
- **Example apps measured**: `safeboot_ecu` (CAN/ISO-TP path — 5 DIDs,
  3 DTCs) and `basic_ecu_doip` (DoIP path — 5 DIDs, 2 DTCs), the two smallest
  configured examples with a real board target. Not the largest configured
  table in the repo (`ardep_ecu`, 35 DIDs/19 DTCs) — that example targets a
  different, custom board this bench does not have. §2.4 gives the
  structural argument needed to extrapolate to a larger table instead.
- **Method**: DWT cycle-counter instrumentation via Zephyr's `timing_*` API
  (`zephyr/timing/timing.h`, which on Cortex-M reads `DWT_CYCCNT` —
  `arch/arm/core/cortex_m/timing.c`), exactly the counter issue #31 itself
  specifies, through Zephyr's own portable wrapper rather than a raw
  register poke. Stimulus driven with Xaloqi TestLab's `UdsTester`/`DoipBus`
  client (DoIP path) and a hand-built ISO-TP sender modelled on this
  project's own CAN reassembly logic (CAN path) — not a saved TestLab
  campaign file, since the goal here was maximum-size/worst-case-shaped
  requests rather than a representative diagnostic session.

## 2. `uds_server_process_request()`

Measured via `safeboot_ecu` over CAN: 420 trials — `DiagnosticSessionControl`
plus `ReadDataByIdentifier` against every configured DID and one
deliberately-absent DID (forces a full, non-early-exit linear scan of
`s_did_count`).

| | cycles | @ 480 MHz |
|---|---:|---:|
| min | 1,732 | 3.6 µs |
| avg | 2,898 | 6.0 µs |
| **max** | **3,792** | **7.9 µs** |

Converged cleanly: the maximum stopped moving after the first ~50 trials and
held for the remaining ~370.

### 2.1 Well inside the P2 budget

`GEN_P2_SERVER_MAX_MS` (this config) is 50 ms. The measured worst case
(7.9 µs) is ~6,300x inside that budget — the dispatch itself is not the
constraint on this table size; UDS transport/ISO-TP framing overhead
(§3) and any policy-registered flash/crypto work dominate instead.

### 2.2 Why this is fast: `did_database.c` / `dtc_database.c` are O(N) linear scans

```c
for (i = (uint16_t)0U; i < s_did_count; i++) { ... }   // config/did_database.c
for (i = (uint16_t)0U; i < s_dtc_count; i++) { ... }   // config/dtc_database.c
```

No recursion, no dynamic allocation, no early-exit dependent on request
content beyond the match itself (the absent-DID trial above forces the full
scan). `UDS_MAX_DID_COUNT` (64) and `UDS_MAX_DTC_COUNT` (128) in
`core/uds_types.h` are compile-time array-capacity ceilings, not runtime
values every ECU reaches — `s_did_count`/`s_dtc_count` are fixed per build at
whatever `diagnostics_config.yaml` declares, so this ECU's own loop can never
exceed 5 iterations regardless of the ceiling.

### 2.3 Per-DID/per-DTC cost, from these two data points

Two same-service trials at different table depths bound a per-iteration
cost: DID index 0 (`0xF190`) vs. the deliberately-absent probe (index 5, a
full miss) differ by roughly `(3,792 − 1,732) / 5 ≈ 412 cycles` per DID
scanned (~0.86 µs at 480 MHz) — consistent with a tight linear scan over a
small fixed-size struct (id, access flags, callback pointer). This project
does not use `-O3`; a `-Os` build's per-iteration cost is what's reported
here.

### 2.4 Extrapolation formula — for a table size this bench cannot build

No integrator can reuse another target's WCET number directly (it is
re-measured per build), so the reusable artefact is the formula, not just
this ECU's own 5-entry figure:

```
WCET(N) ≈ fixed_dispatch_overhead + N × per_iteration_cost
        ≈ (1,732 − 5×412) + N × 412 cycles
        ≈ (-328, i.e. ~0) + N × 412 cycles        [only 2 data points — see caveat]
```

At the compiled ceiling (`UDS_MAX_DID_COUNT = 64`):
`WCET(64) ≈ 64 × 412 ≈ 26,400 cycles ≈ 55 µs` — still ~900x inside the 50 ms
P2 budget. **Caveat**: this line is fit from exactly two measured points on
one 5-entry table; it is a structural argument (the loop is provably O(N),
so a linear formula is the right SHAPE), not a statistically-fit worst-case
bound. An integrator with a materially larger table should re-run this same
campaign against their own build rather than trust the two-point slope here
verbatim.

## 3. `isotp_process_rx_frame()` — multi-frame reassembly

Measured via `safeboot_ecu` over CAN: ~53,000 trials (one call per received
CAN frame) across two stimulus phases — the 420 single-frame requests from
§2 (each isotp call for those is trivially small), and 95 near-maximum
multi-frame messages (4,094-byte payload, `SID 0xFF` so dispatch itself is a
fast NRC and does not dominate the figure) — `UDS_MAX_PAYLOAD_LEN` is 4,095
bytes, so this is the largest single ISO-TP message this stack accepts,
requiring 585 Consecutive Frames to reassemble.

| | cycles | @ 480 MHz |
|---|---:|---:|
| min (mid-message CF) | 322 | 0.67 µs |
| avg (mixed FF/CF/dispatch) | ~21,700–80,500¹ | — |
| **max** (final CF of a 4,094 B message, incl. nested dispatch) | **152,310** | **317 µs** |

¹ The running average is dominated by which mix of frame types happened to
be in the sample window (this instrumentation logs on every new maximum, not
at fixed intervals with independent samples) — the min/max are the load-
bearing figures, not this column.

### 3.1 Convergence

The observed maximum climbed through the first ~4,700 trials
(127,956 → 149,138 cycles) as message-boundary effects (first CF of a
message vs. steady mid-message CFs) got sampled, then a further, smaller
step to 152,310 partway through a dedicated 80-message batch, after which it
held flat for the batch's remaining ~176 logged checkpoints (spanning tens
of thousands of individual `isotp_process_rx_frame()` calls). Treat 152,310
cycles as an **empirically observed maximum across ~53,000 trials**, not a
formally proven upper bound — see §3.3 for what a formal bound would need.

### 3.2 What the final-CF figure includes

`isotp_process_rx_frame()`'s own reassembly work (buffer copy, sequence
counter check, completion detection) is the `min`/typical-CF figure
(322–696 cycles, ~0.7–1.5 µs) — visible directly in the steady mid-message
samples (`cur=692`..`696` throughout the campaign). The **First Frame**
costs more (~80,000–81,500 cycles, ~167–170 µs) — buffer/state setup for a
new reassembly. The **final Consecutive Frame** of a message costs the most
(~149,000–152,000 cycles) because it triggers `on_isotp_rx_complete()` →
`uds_server_process_request()` synchronously, nested inside the same call —
for `SID 0xFF` that's mostly response-building and CAN TX, not table lookup
(§2 already bounds the lookup cost separately for a real service).

### 3.3 Caveat: DWT_CYCCNT is a 32-bit free-running counter

A later, much longer combined run (all three phases back-to-back, several
tens of seconds of continuous wall-clock time) produced an obviously
corrupted `max` (`~1.8×10^19`, near `UINT64_MAX`) — consistent with
`DWT_CYCCNT` wrapping its 32-bit range (~8.9 s at 480 MHz) mid-sample and
`timing_cycles_get()` not correcting for it across a long-lived measurement
window. The figures reported above come from two **shorter** campaigns (each
comfortably under the wrap period) that did not hit this, cross-checked
against each other (independent runs agree on 149,138 → 152,310 cycles as
the plateau). A production WCET campaign run continuously for longer than
~8 s per measurement window should either reset `DWT_CYCCNT` periodically or
accumulate a 64-bit extended counter across wraps — this repo's
instrumentation does neither, since it is measurement scaffolding, not
shipped code.

## 4. DoIP diagnostic message forwarding (`doip_handle_frame()`)

Measured via `basic_ecu_doip` over real Ethernet (STM32H753ZI's onboard MAC
+ PHY, RMII, 100 Mb full duplex — no board devicetree changes needed, the
NUCLEO-H753ZI's Zephyr board support already wires this; only application-
level static-IP Kconfig was added, see
`examples/basic_ecu_doip/boards/nucleo_h753zi/`). 300 trials —
`ReadDataByIdentifier` against every configured DID plus one absent DID,
over one persistent TCP connection.

| | cycles | @ 480 MHz |
|---|---:|---:|
| min (steady state) | 49,360 | 103 µs |
| steady-state max | 137,710 | 287 µs |
| avg (steady state) | ~135,700–137,000 | ~283–285 µs |
| **first-message-on-connection outlier** | **351,870** | **733 µs** |

### 4.1 The cold-start outlier is real, and reportable separately

Trial #1 on a fresh connection cost 351,870 cycles; no trial after it ever
exceeded 137,710 across the remaining 299. This is consistent with one-time
connection/state setup rather than a per-message cost that would recur on a
long-lived diagnostic session — both numbers are reported because both are
real worst cases, for different scenarios (opening a session vs. staying in
one).

### 4.2 What this figure includes

Unlike the CAN-path figures in §2/§3 (measured separately),
`doip_handle_frame()` was measured as one combined figure covering address
validation, the positive ACK send, `uds_server_process_request()` dispatch,
and DoIP response encoding/send — i.e. it already includes an instance of
the §2 dispatch cost. §2's isolated 7.9 µs max is small enough relative to
this path's 287 µs steady-state max that TCP/socket-layer overhead
(`tcp_recv`/`send` inside the Zephyr net stack, not this project's own code)
plausibly dominates over the UDS dispatch itself here — this repo's
instrumentation does not further decompose the net-stack portion.

## 5. Stack depth

`CONFIG_THREAD_STACK_INFO=y` is already the default in both examples'
`prj.conf` (not a WCET-specific addition). Two independent figures:

### 5.1 Static (`-fstack-usage`, already enabled in these examples' `CMakeLists.txt`)

Traced through the `on_isotp_rx_complete()` → `uds_server_process_request()`
→ service handler → safety-wrapper chain (`.su` files from the instrumented
build):

| Frame | Bytes | Class |
|---|---:|---|
| `on_isotp_rx_complete` (main.c) | 136 | dynamic, bounded |
| `uds_server_process_request` (core/uds_server.c) | 64 | static |
| `uds_service_0x2F_handler` (deepest single handler, `WriteMemoryByAddress`) | 128 | static |
| `uds_safety_validate_did_access` (deepest safety-wrapper call this handler makes) | 32 | static |
| **Traced subtotal** | **360** | |

`examples/safeboot_ecu/src/main.c`'s own header comment separately documents
a prior, more deeply-traced static estimate of **~1,600 bytes** for this
same chain (predating this issue's work) — that figure was not
independently re-derived call-by-call here; the 360-byte subtotal above is
what this specific campaign traced and can cite directly. Treat 1,600 bytes
as the standing conservative design figure and 360 bytes as a partial,
independently-confirmed lower bound on the same chain, not a contradiction —
the gap is unexplored deeper frames (DID/DTC generated callback bodies,
CRC/CMAC helpers on other SIDs), not a discrepancy in what was actually
measured.

### 5.2 Dynamic (runtime high-water-mark, `k_thread_stack_space_get()`)

Logged periodically (every 2,000 poll-loop ticks) during the full §2+§3
stimulus campaign (session control, DID reads, and 95 near-maximum
multi-frame messages):

```
diag_task stack: used=688/4096 bytes  (peak observed)
```

Lower than the 1,600-byte static design figure, as expected — the runtime
campaign did not specifically stress `SecurityAccess` (AES-128-CMAC,
`core/uds_aes_cmac.c`) or the DTC-heavy `0x19` service, both plausible
contributors to the static estimate's extra margin. `CONFIG_DIAG_TASK_STACK_SIZE`
is 4,096 bytes by default — even against the more conservative 1,600-byte
static figure that leaves a 2.5x margin (already noted in `main.c`'s header
before this issue); against the 688-byte runtime observation the margin is
~6x for this specific stimulus shape.

## 6. Summary table

| Path | Worst case (cycles) | Worst case (µs @ 480 MHz) | Budget | Margin |
|---|---:|---:|---|---|
| `uds_server_process_request()` (5 DIDs) | 3,792 | 7.9 | P2 = 50 ms | ~6,300x |
| `uds_server_process_request()` (extrapolated, 64 DIDs) | ~26,400 | ~55 | P2 = 50 ms | ~900x |
| `isotp_process_rx_frame()` (4,094 B message, final CF) | 152,310 | 317 | P2 = 50 ms | ~158x |
| DoIP forward, steady state | 137,710 | 287 | P2 = 50 ms | ~174x |
| DoIP forward, first-message-on-connection | 351,870 | 733 | P2 = 50 ms | ~68x |
| Stack (diag_task, runtime observed) | — | — | 4,096 B | ~6x |
| Stack (diag_task, static design figure) | — | — | 4,096 B | 2.5x |

Every measured path stays inside the reference configuration's P2 timing
budget by at least ~68x, including the DoIP cold-start outlier — none of
these paths is close to being the bottleneck against P2/P2*, on this table
size and this hardware.

## 7. What this is not

- Not a formally proven WCET bound (§3.1, §3.3) — an empirically observed
  maximum across real, large-but-finite trial counts on specific stimulus
  shapes, cross-checked where practical against the code's own structural
  guarantees (§2.2: no recursion, no dynamic allocation, provably-O(N)
  loops, compile-time-bounded arrays).
- Not measured at the compiled `UDS_MAX_DID_COUNT`/`UDS_MAX_DTC_COUNT`
  ceiling (§2.4) — no example in this repo populates a table anywhere near
  64/128 entries on a board this bench has. The extrapolation formula is the
  reusable artefact; re-run this campaign against a real large-table build
  for a number instead of a formula.
- Not independently re-verifying the ~1,600-byte static stack estimate
  already in `main.c`'s header (§5.1) — this campaign traced a partial chain
  and cross-checked it against a real runtime high-water-mark instead.

## 8. Reproducing this campaign

```sh
# CAN path (uds_server_process_request + isotp_process_rx_frame):
west build -b nucleo_h753zi examples/safeboot_ecu -- \
    -DDIAG_SKIP_CODEGEN=ON -DEXTRA_CFLAGS=-DDIAG_WCET_MEASURE=1 \
    "-DEXTRA_CONF_FILE=examples/safeboot_ecu/boards/nucleo_h753zi/nucleo_h753zi.conf;\
examples/safeboot_ecu/boards/nucleo_h753zi/wcet_measurement.conf" \
    -DDTC_OVERLAY_FILE=examples/safeboot_ecu/boards/nucleo_h753zi/nucleo_h753zi.overlay
# sign + flash as documented in examples/safeboot_ecu/README.md, then watch
# the serial console for "[WCET]" lines while driving stimulus over CAN.

# DoIP path (doip_handle_frame):
west build -b nucleo_h753zi examples/basic_ecu_doip -- \
    -DDIAG_SKIP_CODEGEN=ON -DDIAG_USE_MOCK_NVM=ON \
    -DEXTRA_CFLAGS=-DDIAG_WCET_MEASURE=1 \
    "-DEXTRA_CONF_FILE=examples/basic_ecu_doip/boards/nucleo_h753zi/nucleo_h753zi_doip.conf;\
examples/basic_ecu_doip/boards/nucleo_h753zi/wcet_measurement.conf"
# flash directly (west flash / openocd — no MCUboot involved), connect a
# host NIC with a static IP on the same /24 as CONFIG_NET_CONFIG_MY_IPV4_ADDR,
# then watch the console for "[WCET]" lines while driving stimulus over DoIP.
```

No UDS-visible readout was added for either campaign — results are read
directly off the serial console (`[WCET]` log lines), not via a debug
DID/routine or a live SWD memory read. A GDB read was tried first and
rejected: every SWD attach path this board's Cortex-M7 accepts needs a
reset first (a DBGMCU examine timing quirk seen throughout this project's
hardware bring-up), which wipes the RAM-resident accumulator being read.

## 9. Traceability

- Issue: #31
- Related: #277 (board substitution rationale, NUCLEO-H743ZI2 → H753ZI),
  #278 (flash layout), #312/#313 (the CAN-path hardware bring-up this
  campaign's stimulus tooling was built on top of)
- Safety Manual gap this closes: `docs/Safety_Model.md` §12,
  "WCET analysis on cross-compiled Cortex-M7 binary at -Os"
