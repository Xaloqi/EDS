# basic_ecu_doip — Xaloqi EDS DoIP Example (Zephyr)

**EDS version:** v1.6.0  
**Transport:** DoIP (ISO 13400-2) over Ethernet/TCP  
**Platform:** Zephyr RTOS — `native_sim` (CI) and any Zephyr Ethernet board  
**DIDs:** 5 · **DTCs:** 2 · **Routines:** 3

---

## What this example demonstrates

The same diagnostic interface as `basic_ecu` — VIN, ECU serial, part number, engine speed,
coolant temperature, two DTCs, and three routines — served over DoIP instead of CAN/ISO-TP.

The UDS stack, ASIL-B safety wrappers, security access (AES-128-CMAC), DTC persistence, and
YAML schema are identical to `basic_ecu`. The only differences are:

- `ecu.transport: doip` in `diagnostics_config.yaml`
- `eds_doip_platform_start()` called in `main.c` instead of the CAN diagnostic task
- Zephyr networking Kconfig enabled (`CONFIG_NETWORKING=y`, `CONFIG_NET_TCP=y`)
- `EDS_DOIP_ONLY_BUILD=1`, set by `CMakeLists.txt` (not passed on the command line) — omits ISO-TP init from generated `uds_init.c`

This is the canonical reference for adding DoIP to any Zephyr EDS ECU.

---

## YAML transport configuration

```yaml
ecu:
  transport: doip           # "can" (default) | "doip" | "both"
  doip:
    logical_address: "0xE400"   # This ECU's DoIP logical address
    source_address:  "0x0E00"   # Expected tester source address (xaloqi-tester default)
    port:            13400       # Standard DoIP TCP port (ISO 13400)
```

Existing `diagnostics_config.yaml` files without an `ecu:` block are unchanged —
`transport` defaults to `"can"`.

---

## Building

### native_sim (CI — loopback networking, no hardware)

```bash
west build -b native_sim examples/basic_ecu_doip \
  -- \
  -DEXTRA_CONF_FILE="${PWD}/examples/basic_ecu_doip/boards/native_sim/native_sim_doip.conf" \
  -DDIAG_SKIP_CODEGEN=ON \
  -DZEPHYR_TOOLCHAIN_VARIANT=host
```

> **Note:** `-DDIAG_SKIP_CODEGEN=ON` builds from the pre-generated files committed in
> `generated/`. Without it the build runs codegen, which needs the Jinja2 templates from the
> EDS toolchain; a public clone does not have them, so the build fails. With the toolchain
> installed you can drop it, and codegen produces identical output.

### Hardware (any Zephyr Ethernet board)

```bash
# Example: FRDM-K64F
west build -b frdm_k64f examples/basic_ecu_doip \
  -- -DDIAG_SKIP_CODEGEN=ON -DDIAG_USE_MOCK_NVM=ON
west flash
```

`DIAG_USE_MOCK_NVM=ON` keeps DTC storage in RAM. Without it the board needs a
flash partition for the NVM backend, which this example does not provide
([#408](https://github.com/Xaloqi/EDS/issues/408)). This build is
compile-checked; the NUCLEO-H753ZI setup below is the hardware-verified one.

Ensure your board DTS includes an Ethernet driver and that `CONFIG_NETWORKING=y` is
satisfied. The DoIP server listens on port 13400 immediately after boot.

### NUCLEO-H753ZI (real Ethernet, hardware-verified bench setup)

```bash
B=examples/basic_ecu_doip/boards/nucleo_h753zi
west build -b nucleo_h753zi examples/basic_ecu_doip -- \
  -DDIAG_SKIP_CODEGEN=ON -DDIAG_USE_MOCK_NVM=ON \
  -DEXTRA_CONF_FILE=$PWD/$B/nucleo_h753zi_doip.conf \
  -DEXTRA_DTC_OVERLAY_FILE=$PWD/$B/nucleo_h753zi_doip.overlay
west flash
```

- **Network:** static `10.10.10.1/24` on a direct cable to the host. Give
  the host an address on the same subnet, e.g.
  `sudo ip addr add 10.10.10.2/24 dev <iface>`. The console is the
  ST-LINK virtual COM port at 115200.
- **Storage is RAM-backed on this board (supported bench setup).**
  `DIAG_USE_MOCK_NVM=ON` keeps the DTC mirror in RAM, so it does not
  survive a reset. Without it the build does not link: the conf provides
  no flash partition for the real backend
  ([#403](https://github.com/Xaloqi/EDS/issues/403)). Real flash storage
  with the append backend is tracked in
  [#408](https://github.com/Xaloqi/EDS/issues/408).
- **Watchdog:** the overlay enables the IWDG. `main()` arms it and the
  UDS tick task feeds it only while the DoIP server keeps checking in
  ([#399](https://github.com/Xaloqi/EDS/issues/399)). The comment at the
  top of `src/main.c` lists exactly what is supervised.

### NUCLEO-H753ZI firmware-download bench (bench only)

Adds `-DDIAG_DOIP_FLASH_BENCH=ON` to the command above, so that
RequestDownload / TransferData / RequestTransferExit write **real flash**
over DoIP ([#423](https://github.com/Xaloqi/EDS/issues/423)):

```bash
B=examples/basic_ecu_doip/boards/nucleo_h753zi
west build -b nucleo_h753zi examples/basic_ecu_doip -- \
  -DDIAG_SKIP_CODEGEN=ON -DDIAG_USE_MOCK_NVM=ON -DDIAG_DOIP_FLASH_BENCH=ON \
  -DEXTRA_CONF_FILE=$PWD/$B/nucleo_h753zi_doip.conf \
  -DEXTRA_DTC_OVERLAY_FILE=$PWD/$B/nucleo_h753zi_doip.overlay
west flash
```

- **Target:** a 1 MiB `image_1` partition in flash bank 2
  (`nucleo_h753zi_flash_bench.overlay`), written by
  `platform/zephyr/zephyr_flash_ops.c`. RequestDownload addresses are
  partition-relative flash offsets: `0x00100000`..`0x001FFFFF`.
- **Sequence:** 0x10 03, 0x10 02, SecurityAccess level 1, then
  `34 00 44 <addr> <size>`. The ECU answers 0x78 while it erases one
  128 KB sector per step, then `74`. Then 0x36 blocks
  (`maxNumberOfBlockLength - 2` data bytes each), and `37` with the image's
  CRC-32 (optional). The 0x37 response follows the ECU's read-back CRC check.
- **Measured** (1 MiB, one connection): erase 6.6 s, 4113 blocks in 7.7 s
  (~133 KiB/s), 0x37 in 0.14 s. The flash read back over SWD matches the
  image byte for byte, and the watchdog stays clean.
- **Bench only:** off by default, refused for other boards, and nothing
  boots from the written image. Booting a downloaded, signed image is
  [#425](https://github.com/Xaloqi/EDS/issues/425).

---

## Running

Start the ECU binary in one terminal:

```bash
./build/zephyr/zephyr.exe
# Zephyr starts, DoIP thread binds to 127.0.0.1:13400
```

Connect with xaloqi-tester (Xaloqi TestLab) in another:

```python
import asyncio
from xaloqi.tester import UdsTester, DoipBus

async def main():
    async with UdsTester(
        DoipBus("127.0.0.1"),
        rx_id=0xE400,   # ECU logical address
        tx_id=0x0E00,   # Tester source address
    ) as ecu:
        vin = await ecu.read_did(0xF190)
        print("VIN:", vin)

asyncio.run(main())
```

Or run the full integration test suite:

```bash
DOIP_ECU_BINARY=build/zephyr/zephyr.exe \
pytest tests/test_doip_integration.py -v --timeout=60
```

---

## Thread model

```
main()
  └── uds_generated_init(NULL, 0, 0)   — UDS stack init (no CAN transport)
  └── eds_doip_platform_start(         — registers DoIP ops, starts thread
        logical_addr = 0xE400,
        port         = 13400,
        uds_server   = uds_generated_get_server()
      )
  └── returns

doip_thread  (K_THREAD_DEFINE in zephyr_lwip.c)
  └── eds_doip_server_run()            — TCP accept loop, frame dispatch
```

The `diag_task` CAN thread from `basic_ecu` is not started here. For a dual-transport
ECU (`ecu.transport: both`), add the CAN task alongside the DoIP init.

---

## File structure

```
basic_ecu_doip/
├── diagnostics_config.yaml     YAML source — 5 DIDs, 2 DTCs, 3 routines, transport: doip
├── CMakeLists.txt              Zephyr CMake — DoIP sources, EDS_DOIP_ONLY_BUILD flag
├── prj.conf                    Base Kconfig — threading, networking, no heap
├── boards/
│   └── native_sim/
│       └── native_sim_doip.conf   native_sim Kconfig overlay — POSIX_API, loopback net
├── src/
│   └── main.c                  Application entry point — init, DoIP start
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

## Key differences from `basic_ecu`

| | `basic_ecu` | `basic_ecu_doip` |
|---|---|---|
| Transport | ISO-TP over CAN | DoIP over Ethernet/TCP |
| `uds_generated_init` call | `init(can, rx_id, tx_id)` | `init(NULL, 0, 0)` |
| CAN diagnostic task | Started in `main.c` | Not present |
| DoIP platform start | Not present | `eds_doip_platform_start()` |
| Build define | — | `EDS_DOIP_ONLY_BUILD=1` (set in `CMakeLists.txt`) |
| YAML difference | No `ecu:` block | `ecu.transport: doip` |
| UDS core, safety, security | Identical | Identical |
| Generated DID/DTC/routine files | Identical structure | Identical structure |

---

## Regenerating the generated files

```bash
python3 tools/codegen.py \
  --config examples/basic_ecu_doip/diagnostics_config.yaml \
  --out    examples/basic_ecu_doip/generated/ \
  --safety-wrappers --asil-level B --no-manifest
```

Requires the EDS Developer or Professional toolchain (Jinja2 templates in `tools/templates/`).

---

## See also

- `examples/basic_ecu/` — CAN/ISO-TP baseline with the same DID/DTC set
- `examples/basic_ecu_doip_freertos/` — FreeRTOS + LwIP counterpart to this example
- `docs/INTEGRATION_GUIDE.md` Section 5b — complete DoIP integration walkthrough
- `tests/test_doip_integration.py` — 10 end-to-end pytest tests against this ECU
- `transport/doip/doip_server.h` — DoIP server API documentation
