# basic_ecu_doip — Xaloqi EDS DoIP Example (Zephyr)

**EDS version:** v1.6.0  
**Transport:** DoIP (ISO 13400-2) over Ethernet/TCP  
**Platform:** Zephyr RTOS — `native_sim` (CI) and any Zephyr Ethernet board  
**DIDs:** 5 · **DTCs:** 2 · **Routines:** 3

---

## What this example demonstrates

The same diagnostic interface as `basic_ecu` — VIN, ECU serial, part number, engine speed,
coolant temperature, two DTCs, and three routines — served over DoIP instead of CAN/ISO-TP.

The UDS stack, ASIL-B-oriented safety wrappers, security access (AES-128-CMAC), DTC persistence, and
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
`diag_nvs` flash partition in its devicetree (the NUCLEO-H753ZI and
FRDM-MCXN947 setups below pass one). This build is compile-checked; the
NUCLEO-H753ZI and FRDM-MCXN947 setups below have carried DoIP traffic on
hardware.

Ensure your board DTS includes an Ethernet driver and that `CONFIG_NETWORKING=y` is
satisfied. The DoIP server listens on port 13400 immediately after boot.

### NUCLEO-H753ZI (real Ethernet, real flash storage)

```bash
B=examples/basic_ecu_doip/boards/nucleo_h753zi
west build -b nucleo_h753zi examples/basic_ecu_doip -- \
  -DDIAG_SKIP_CODEGEN=ON \
  -DEXTRA_CONF_FILE=$PWD/$B/nucleo_h753zi_doip.conf \
  -DDTC_OVERLAY_FILE=$PWD/boards/nucleo_h753zi/nucleo_h753zi.overlay \
  -DEXTRA_DTC_OVERLAY_FILE=$PWD/$B/nucleo_h753zi_doip.overlay
west flash
```

- **Network:** static `10.10.10.1/24` on a direct cable to the host. Give
  the host an address on the same subnet, e.g.
  `sudo ip addr add 10.10.10.2/24 dev <iface>`. The console is the
  ST-LINK virtual COM port at 115200.
- **Storage:** the DTC mirror and the SecurityAccess counters are in the
  board's `diag_nvs` flash partition (256 KB at `0x1C0000`, from
  `boards/nucleo_h753zi/nucleo_h753zi.overlay`), written with the append
  backend: two 128 KB banks, because NVS cannot mount the STM32H7's 128 KB
  sectors ([#408](https://github.com/Xaloqi/EDS/issues/408)). **Not yet run
  on hardware:** a DTC set before a real reset reading back after it, and a
  boot without a reset loop on first-boot formatting, are the checks that
  remain. For a RAM-backed mirror (no partition needed, does not survive a
  reset) add `-DDIAG_USE_MOCK_NVM=ON` and drop `DTC_OVERLAY_FILE`
  ([#403](https://github.com/Xaloqi/EDS/issues/403)); the firmware-download
  bench below keeps that setting.
- **Watchdog:** the overlay enables the IWDG. `main()` arms it before the NVM
  store opens, so the append backend can feed it across a bank erase, and the
  UDS tick task feeds it only while the DoIP server keeps checking in
  ([#399](https://github.com/Xaloqi/EDS/issues/399)). The comment at the
  top of `src/main.c` lists exactly what is supervised.

### FRDM-MCXN947 (on-board Ethernet, real flash storage)

NXP MCX N947 is an industrial-grade MCU.

```bash
west build -b frdm_mcxn947/mcxn947/cpu0 examples/basic_ecu_doip -- \
  -DDIAG_SKIP_CODEGEN=ON \
  "-DEXTRA_CONF_FILE=$PWD/boards/frdm_mcxn947/frdm_mcxn947.conf;$PWD/examples/basic_ecu_doip/boards/frdm_mcxn947/frdm_mcxn947_doip.conf" \
  -DDTC_OVERLAY_FILE=$PWD/boards/frdm_mcxn947/frdm_mcxn947.overlay
```

The storage is the board's `diag_nvs` partition with the append backend (the
128-byte write block is more than NVS takes, [#640](https://github.com/Xaloqi/EDS/issues/640)).
**Run on hardware** (2026-11-23): the DoIP Pro campaign 8/8 before and after a
reset, and the SecurityAccess attempt counter kept in `diag_nvs` over the
reset pin. The conf sets `CONFIG_NET_BUF_DATA_SIZE=1536`: with Zephyr v3.7.0's
ENET QoS driver a received frame must fit one network buffer, and the default
128 bytes made any longer frame panic the board
([#647](https://github.com/Xaloqi/EDS/issues/647)); it costs about 99 KB of
RAM. Frames over 1512 bytes are still dropped by that driver
([#648](https://github.com/Xaloqi/EDS/issues/648)), so keep DoIP transfers
below full-size TCP segments for now. The TCP sequence
numbers come from Zephyr's test random generator (see the conf), so this is a
bench setup, not a product one.

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
    ├── did_safety_wrappers.c/.h   ASIL-B-oriented 5-step wrappers
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
