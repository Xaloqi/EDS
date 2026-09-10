# basic_ecu_doip_freertos — Xaloqi EDS DoIP Example (FreeRTOS)

**EDS version:** v1.6.0  
**Transport:** DoIP (ISO 13400-2) over Ethernet/TCP  
**Platform:** FreeRTOS + LwIP — any Ethernet-capable MCU (STM32H7, i.MX RT, etc.)  
**CI target:** QEMU `mps2-an386` (Cortex-M4) — real DoIP/TCP over an emulated LAN9118 MAC when `LWIP_DIR` is set; compile-only otherwise  
**DIDs:** 5 · **DTCs:** 2 · **Routines:** 3

---

## What this example demonstrates

The FreeRTOS counterpart to `basic_ecu_doip`. Identical UDS interface, ASIL-B safety wrappers,
security access, and YAML configuration — running on FreeRTOS + LwIP instead of Zephyr.

The integration sequence is four calls:

```c
eds_platform_init(&cfg);                            // CAN shim + NVM (unused for DoIP-only)
uds_generated_init(NULL, 0U, 0U);                  // UDS stack — no CAN transport
eds_doip_platform_start_freertos(                   // DoIP server task (xTaskCreate)
    0xE400U, DOIP_PORT, uds_generated_get_server(),
    4096U, 6U);
vTaskStartScheduler();
```

This example demonstrates that the same YAML, same templates, and same UDS core compile
unmodified for FreeRTOS. Only the OS layer changes.

---

## YAML transport configuration

```yaml
ecu:
  transport: doip
  doip:
    logical_address: "0xE400"
    source_address:  "0x0E00"
    port:            13400
```

---

## Building

There are two QEMU builds: a **compile-only** one (the default) and a
**runnable, network-capable** one selected by setting `LWIP_DIR`.

### QEMU ARM Cortex-M4 — compile test only (default)

```bash
cmake -B build \
  -DEDS_PLATFORM=freertos \
  -DFREERTOS_DIR=/path/to/FreeRTOS-Kernel \
  -GNinja \
  examples/basic_ecu_doip_freertos

ninja -C build
```

With no `LWIP_DIR`, the stub headers under `boards/qemu_cortex_m4/lwip_stub/`
satisfy the include dependencies without bringing in lwIP. Every socket call
in that stub returns `-1` by design, so the binary builds and boots but can
never accept a DoIP connection. Use it to check that the UDS stack compiles
for FreeRTOS — not to test DoIP.

### QEMU ARM Cortex-M4 — real DoIP over emulated Ethernet

This is what the `xaloqi-compatibility-tests` `basic_ecu_doip_freertos` leg
runs. lwIP is fetched, not vendored — the same convention as
`FREERTOS_DIR`:

```bash
git clone --depth=1 -b STABLE-2_2_1_RELEASE \
  https://github.com/lwip-tcpip/lwip.git /opt/lwip

cmake -B build \
  -DEDS_PLATFORM=freertos \
  -DFREERTOS_DIR=/path/to/FreeRTOS-Kernel \
  -DLWIP_DIR=/opt/lwip \
  -GNinja \
  examples/basic_ecu_doip_freertos

ninja -C build
```

Run it, with the guest's port 13400 forwarded to the host:

```bash
qemu-system-arm \
  -machine mps2-an386 \
  -kernel build/eds_freertos_doip.elf \
  -net nic,model=lan9118 \
  -net user,hostfwd=tcp::13400-:13400 \
  -nographic -serial file:boot.log
```

`mps2-an386` is the correct machine: it is QEMU's **Cortex-M4** MPS2 variant,
matching this example's `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16` build and the
memory map in `boards/qemu_cortex_m4/linker.ld`. It emulates an SMSC
**LAN9118** MAC at `0x40200000`, which
`boards/qemu_cortex_m4/lwip_port/ethernetif_lan9118.c` drives. (`lm3s6965evb`
is a Cortex-M3 machine with a different MAC and a different memory map — a
Cortex-M4 hard-float image will not run on it.)

`boot.log` should show the network coming up:

```
[eds] basic_ecu_doip_freertos boot
[eds] board=qemu mps2-an386 cortex-m4f
[eds] platform init ok
[eds] uds stack init ok
[eds] doip task created, starting scheduler
[net] tcpip_init...
[net] tcpip up, lwip 2.2.1
[net] mac  02:00:00:ed:50:01
[net] chip id_rev 01180001
[net] ip   10.0.2.15
[net] mask 255.255.255.0
[net] gw   10.0.2.2
[net] netif up — DoIP reachable on port 13400
```

`chip id_rev 01180001` is read back from the emulated MAC, so that line is
positive proof the driver reached real hardware registers.

> **Do not use a bare TCP connect as a liveness check.** Connecting to the
> forwarded host port succeeds even against the stub build, because QEMU's
> slirp `hostfwd` accepts on the host side before it can know whether a guest
> listener exists. Only a completed DoIP exchange proves anything.

### STM32H7 (hardware target)

```cmake
# In CMakeLists.txt or cmake call, set:
-DFREERTOS_DIR=/path/to/FreeRTOS-Kernel
-DLWIP_DIR=/path/to/lwip
-DEDS_DOIP_ONLY_BUILD=ON
-DTARGET_MCU=STM32H743
```

Replace the LwIP netif and Ethernet driver initialisation stubs in `src/main.c` with your
board's Ethernet HAL. The DoIP server task starts listening on port 13400 once
`netif_set_up()` completes.

---

## Thread model

```
main()
  └── eds_platform_init()                            — register CAN + NVM callbacks
  └── uds_generated_init(NULL, 0U, 0U)              — UDS stack (no CAN transport)
  └── eds_doip_platform_start_freertos(              — xTaskCreate for DoIP server
        logical_addr  = 0xE400,
        port          = 13400,
        uds_server    = uds_generated_get_server(),
        stack_depth   = 4096,
        priority      = 6
      )
  └── vTaskStartScheduler()

doip_task  (created by eds_doip_platform_start_freertos)
  └── eds_doip_server_run()                          — LwIP socket accept loop
```

---

## Runtime testing

With LwIP connected to real Ethernet, connect with xaloqi-tester:

```python
import asyncio
from xaloqi.tester import UdsTester, DoipBus

async def main():
    async with UdsTester(
        DoipBus("192.168.1.100"),   # MCU IP address
        rx_id=0xE400,
        tx_id=0x0E00,
    ) as ecu:
        vin = await ecu.read_did(0xF190)
        print("VIN:", vin)

asyncio.run(main())
```

---

## File structure

```
basic_ecu_doip_freertos/
├── diagnostics_config.yaml     YAML source — 5 DIDs, 2 DTCs, 3 routines, transport: doip
├── CMakeLists.txt              FreeRTOS CMake — DoIP sources, EDS_DOIP_ONLY_BUILD flag
├── boards/
│   └── qemu_cortex_m4/
│       ├── startup.c           ARMv7-M vector table + C runtime startup
│       ├── freertos_hooks.c    Static-allocation callbacks (idle task memory)
│       ├── board_uart.c/.h     Polled CMSDK UART0 — boot logging
│       ├── linker.ld           mps2-an386 memory map
│       ├── FreeRTOSConfig.h    1 ms tick, 48 KB heap
│       ├── lwip_stub/          Minimal LwIP headers — compile-only default
│       └── lwip_port/          Real LwIP port (used when LWIP_DIR is set)
│           ├── lwipopts.h              IPv4 + sockets, static pools
│           ├── arch/cc.h, sys_arch.h   Compiler/OS abstraction types
│           ├── sys_arch.c              lwIP NO_SYS=0 layer on FreeRTOS
│           └── ethernetif_lan9118.c/.h QEMU LAN9118 netif driver
├── src/
│   └── main.c                  Integration sequence — eds_platform_init → DoIP start
└── generated/                  Pre-generated C files (committed, updated by codegen)
    ├── uds_init.c              EDS_DOIP_ONLY_BUILD guards — isotp_init() omitted
    ├── uds_init.h              DoIP function prototype variant
    ├── did_handlers.c/.h       DID read/write stubs
    ├── did_safety_wrappers.c/.h   ASIL-B 5-step wrappers
    ├── routine_handlers.c/.h   Routine stubs
    ├── generated_config.h      Compile-time constants
    └── safety_config.h         ASIL-B _Static_assert guards
```

---

## Key differences from `basic_ecu_freertos`

| | `basic_ecu_freertos` | `basic_ecu_doip_freertos` |
|---|---|---|
| Transport | ISO-TP over CAN | DoIP over Ethernet/TCP |
| `uds_generated_init` call | `init(can, rx_id, tx_id)` | `init(NULL, 0, 0)` |
| CAN diagnostic task | Started via `eds_freertos_start()` | Not present |
| DoIP platform start | Not present | `eds_doip_platform_start_freertos()` |
| Build flag | — | `-DEDS_DOIP_ONLY_BUILD=ON` |
| LwIP dependency | Not required | Required at runtime |
| YAML difference | No `ecu:` block | `ecu.transport: doip` |
| UDS core, safety, security | Identical | Identical |

---

## Regenerating the generated files

```bash
python3 tools/codegen.py \
  --config examples/basic_ecu_doip_freertos/diagnostics_config.yaml \
  --out    examples/basic_ecu_doip_freertos/generated/ \
  --safety-wrappers --asil-level B --no-manifest
```

Requires the EDS Developer or Professional toolchain.

---

## Platform integration notes

**Customer responsibilities for a production target:**

1. **LwIP Ethernet driver** — replace the `netif_add()` stub in `main.c` with your board's
   Ethernet driver. Call `netif_set_up()` before `eds_doip_platform_start_freertos()`.

2. **FreeRTOS heap** — the DoIP server task uses `configMINIMAL_STACK_SIZE` plus the
   `stack_depth` argument (default 4096 bytes). Add this to your `configTOTAL_HEAP_SIZE`
   estimate.

3. **Task priority** — DoIP server priority defaults to 6. Adjust relative to your
   application tasks. The DoIP task blocks on `lwip_accept()` — it does not busy-wait.

4. **IP addressing** — `main.c` includes a DHCP placeholder. Replace with static
   addressing or your DHCP implementation before calling `eds_doip_platform_start_freertos()`.

---

## See also

- `examples/basic_ecu_doip/` — Zephyr counterpart using `zsock_*` and `K_THREAD_DEFINE`
- `examples/basic_ecu_freertos/` — FreeRTOS baseline with CAN/ISO-TP transport
- `docs/INTEGRATION_GUIDE.md` Section 5b — complete DoIP integration walkthrough (Zephyr and FreeRTOS)
- `platform/freertos/platform_doip.h` — `eds_doip_platform_start_freertos()` API
- `transport/doip/freertos_lwip.h` — LwIP socket binding documentation
