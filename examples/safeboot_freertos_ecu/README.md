# safeboot_freertos_ecu — FreeRTOS OTA DFU Example (Nucleo-H743ZI2)

UDS OTA firmware download over CAN on STM32H743ZI + FreeRTOS, without MCUboot.

FreeRTOS companion to [`examples/safeboot_ecu/`](../safeboot_ecu/) (Zephyr + MCUboot).

> **Flash driver** based on work contributed by chenyurong22 (Siemens) in
> [GitHub issue #28](https://github.com/Xaloqi/EDS/issues/28). Thank you.

---

## What this example demonstrates

The complete UDS firmware download pipeline on a bare FreeRTOS target:

```
UDS tester (TestLab / CANoe / scan tool)
    │
    │  0x10 0x02     DiagnosticSessionControl → programmingSession
    │  0x27 0x01     SecurityAccess → RequestSeed
    │  0x27 0x02     SecurityAccess → SendKey (AES-128-CMAC)
    │  0x31 0xFF00   RoutineControl → CheckProgrammingPreconditions
    │  0x34          RequestDownload (erase Bank 2, returns maxBlockLen=256)
    │  0x36 ×N       TransferData (256-byte blocks, CRC-32 accumulated)
    │  0x37          RequestTransferExit (CRC-32 finalised and verified)
    │  0x31 0xFF01   RoutineControl → VerifyOTASlotIntegrity (optional)
    │  0x11 0x01     ECUReset → hardReset
    │
    ▼
Customer bootloader wakes → swaps Bank 2 → Bank 1 → boots new application
```

**What this example does NOT include:**
- Boot-time bank swap — that is the customer bootloader's responsibility
- A/B swap state machine with rollback counter — available in Developer tier

---

## Flash layout (STM32H743ZI — 2 MB dual-bank)

| Region | Address | Size | Purpose |
|---|---|---|---|
| Bank 1 | 0x08000000 | 1 MB | Running application (8 sectors × 128 KB) |
| Bank 2 — OTA | 0x08100000 | 896 KB | OTA staging area (7 sectors × 128 KB) |
| Bank 2 — NVM | 0x081E0000 | 128 KB | Last sector, reserved for UDS NVM |

The UDS download services write exclusively to Bank 2 (OTA staging).
Address range is validated by `freertos_flash_ops_init()` before any write.

---

## Hardware setup

### Required

| Part | Purpose |
|---|---|
| STM32 Nucleo-H743ZI2 | Target board |
| TJA1051T/3 CAN transceiver (3.3 V) | CAN physical layer |
| USB-CAN adapter (PEAK PCAN-USB, Kvaser Leaf, etc.) | Host-side CAN |
| 2× 120 Ω resistors | CAN bus termination |

### Wiring (FDCAN1)

| Nucleo CN8 | Signal | TJA1051 |
|---|---|---|
| PD0 | FDCAN1_RX | TXD |
| PD1 | FDCAN1_TX | RXD |
| 3V3 | VCC | VCC |
| GND | GND | GND |

Bus: 500 kbit/s, sample point 87.5%.

---

## Prerequisites

- `arm-none-eabi-gcc` ≥ 12
- [FreeRTOS Kernel](https://github.com/FreeRTOS/FreeRTOS-Kernel) cloned locally
- STM32H7 HAL library (`STM32H7xx_HAL_Driver`) from STM32CubeH7 or CubeIDE

---

## Build

### 1. Run codegen

```sh
python3 ../../tools/codegen.py \
    --config diagnostics_config.yaml \
    --out    generated/ \
    --safety-wrappers --asil-level B --no-manifest
```

*(The pre-generated files in `generated/` are committed and ready to use.)*

### 2. Configure CMake (STM32H743ZI hardware)

```sh
cmake -B build \
    -DEDS_PLATFORM=freertos \
    -DFREERTOS_DIR=/path/to/FreeRTOS-Kernel \
    -DBOARD=nucleo_h743zi \
    -DSTM32_HAL_DIR=/path/to/STM32H7xx_HAL_Driver \
    -GNinja .
```

### 3. Build

```sh
ninja -C build
```

Output: `build/eds_safeboot_freertos.elf`

### 4. Flash

```sh
# OpenOCD (ST-LINK on Nucleo):
openocd -f board/st_nucleo_h743zi.cfg \
        -c "program build/eds_safeboot_freertos.elf verify reset exit"

# Or STM32CubeProgrammer:
STM32_Programmer_CLI -c port=SWD -d build/eds_safeboot_freertos.elf -rst
```

### CI build (QEMU Cortex-M4, RAM stub)

```sh
cmake -B build_ci \
    -DEDS_PLATFORM=freertos \
    -DFREERTOS_DIR=/path/to/FreeRTOS-Kernel \
    -DBOARD=qemu_cortex_m4 \
    -GNinja .
ninja -C build_ci
```

No STM32 HAL required. Flash operations run in a RAM stub — useful for
compile testing and basic UDS flow validation in CI.

---

## Adapting to real hardware

Replace the stub CAN loopback in `src/main.c`:

```c
/* Replace loopback_can_send() body with: */
FDCAN_TxHeaderTypeDef tx_hdr = {
    .Identifier          = frame->id,
    .IdType              = FDCAN_STANDARD_ID,
    .TxFrameType         = FDCAN_DATA_FRAME,
    .DataLength          = FDCAN_DLC_BYTES_8,
    /* ... */
};
HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &tx_hdr, frame->data);
```

Wire the FDCAN RX ISR:

```c
void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
    FDCAN_RxHeaderTypeDef hdr;
    eds_can_frame_t frame;
    HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &hdr, frame.data);
    frame.id  = hdr.Identifier;
    frame.dlc = (uint8_t)(hdr.DataLength >> 16U);
    eds_platform_can_input(&frame);
}
```

---

## Performing a DFU update

### With Xaloqi TestLab

```sh
testlab run campaigns/safeboot_freertos_dfu.yaml \
    --firmware path/to/new_app.bin
```

### Manual UDS bytes (500 kbit/s, 0x7DF → 0x7E8)

```
# 1. Programming session
10 02  →  50 02 00 19 01 F4

# 2. RequestSeed
27 01  →  67 01 <4-byte seed>

# 3. SendKey (AES-128-CMAC of seed using level-1 key)
27 02 <4-byte key>  →  67 02

# 4. CheckProgrammingPreconditions
31 01 FF 00  →  71 01 FF 00 01 00

# 5. RequestDownload (ALFID 0x44 = 4-byte addr + 4-byte len)
34 00 44  08 10 00 00  <len3> <len2> <len1> <len0>
→  74 20 01 00                          (maxBlockLen = 256)

# 6. TransferData — repeat for each 256-byte block
36 <blk_seq> <256 bytes>  →  76 <blk_seq>

# 7. RequestTransferExit — CRC-32 verified
37  →  77

# 8. VerifyOTASlotIntegrity (optional)
31 01 FF 01  →  71 01 FF 01 01 00      (PASS)

# 9. Hard reset
11 01  →  51 01  (then device resets, customer bootloader runs)
```

---

## Post-DFU verification

Read DID 0xF181 after reboot to confirm the active image version:

```sh
testlab read-did --did 0xF181
```

---

## DIDs

| DID | Name | Size | Example value |
|---|---|---|---|
| 0xF190 | VehicleIdentificationNumber | 17 B | `XALQ1EDS00SFBT001` |
| 0xF18C | ECUSerialNumber | 8 B | `SFB00001` |
| 0xF181 | ApplicationSoftwareIdentification | 8 B | `v1.0.0\0\0` |
| 0xF186 | ActiveDiagnosticSession | 1 B | `0x01` (default) |
| 0xF18A | SystemSupplierIdentifier | 10 B | `XALOQI    ` |

---

## Routines

| RID | Name | Session | Security | Description |
|---|---|---|---|---|
| 0xFF00 | CheckProgrammingPreconditions | Extended | 0 | Checks supply voltage, safe state |
| 0xFF01 | VerifyOTASlotIntegrity | Programming | 1 | Validates ARM vector table in Bank 2 |

### VerifyOTASlotIntegrity result format

| Byte | Meaning |
|---|---|
| 0 | `0x01` PASS · `0x02` FAIL |
| 1 | `0x00` OK · `0x01` slot erased · `0x02` invalid SP · `0x03` invalid Reset_Handler |

---

## DTCs

| DTC | Trigger |
|---|---|
| 0xF00001 | CRC-32 mismatch on RequestTransferExit |
| 0xF00002 | Flash erase failure during RequestDownload |
| 0xF00003 | Flash write failure during TransferData |

---

## File structure

```
safeboot_freertos_ecu/
├── diagnostics_config.yaml         5 DIDs, 3 DTCs, safeboot.platform: freertos
├── src/main.c                      FreeRTOS main — UDS poll task + CAN stub
├── CMakeLists.txt                  ARM Cortex-M7 + STM32H7 HAL optional
├── campaigns/
│   └── safeboot_freertos_dfu.yaml  TestLab OTA DFU campaign
├── boards/
│   ├── nucleo_h743zi/              Production target (H743ZI2)
│   │   ├── FreeRTOSConfig.h        400 MHz, 1 ms tick
│   │   └── linker.ld               Bank 1 app + Bank 2 OTA layout
│   └── qemu_cortex_m4/            CI target (QEMU, RAM stub flash)
│       ├── FreeRTOSConfig.h
│       └── linker.ld
└── generated/
    ├── uds_init.c                  Step 5.7 calls freertos_flash_ops_init()
    ├── did_handlers.c              5 DID read handlers
    └── routine_handlers.c          0xFF00 preconditions + 0xFF01 slot verify
```

The flash ops driver lives in the platform layer:
```
platform/freertos/
├── freertos_flash_ops.h
└── freertos_flash_ops.c   STM32H743 HAL backend + RAM stub for CI
```

---

## Developer tier: A/B swap state machine

This example writes the new image to Bank 2 and verifies CRC.
The actual bank swap at boot requires a bootloader or the
**EDS Developer tier** A/B swap state machine, which adds:

- Metadata sector (boot flag, rollback counter, swap state)
- N-consecutive-boot rollback guard
- Post-swap self-test hook

See [xaloqi.com](https://xaloqi.com) for Developer licensing.

---

## See also

- [`examples/safeboot_ecu/`](../safeboot_ecu/) — Zephyr + MCUboot equivalent
- `platform/freertos/freertos_flash_ops.c` — STM32H743 HAL flash driver
- `generated/uds_init.c` — full 9-step init sequence including flash ops wiring
- [GitHub issue #28](https://github.com/Xaloqi/EDS/issues/28) — original request + flash driver contribution
