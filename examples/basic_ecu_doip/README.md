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
- `-DEDS_DOIP_ONLY_BUILD` in `CMakeLists.txt` — omits ISO-TP init from generated `uds_init.c`

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
  -DEDS_DOIP_ONLY_BUILD \
  -DZEPHYR_TOOLCHAIN_VARIANT=host
```

> **Note:** `-DDIAG_SKIP_CODEGEN=ON` is used in public CI because the Jinja2 templates
> live in the private EDS-toolchain repo. Omit it when you have the full toolchain installed:
> codegen will re-run automatically and produce identical output.

### Hardware (any Zephyr Ethernet board)

```bash
# Example: FRDM-K64F
west build -b frdm_k64f examples/basic_ecu_doip \
  -- -DEDS_DOIP_ONLY_BUILD
west flash
```

Ensure your board DTS includes an Ethernet driver and that `CONFIG_NETWORKING=y` is
satisfied. The DoIP server listens on port 13400 immediately after boot.

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
| Build flag | — | `-DEDS_DOIP_ONLY_BUILD` |
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
