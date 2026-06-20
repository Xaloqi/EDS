# safeboot_ecu — OTA Bootloader Example (Nucleo-H743ZI)

Secure OTA firmware update over UDS + MCUboot on the STM32H743ZI.
Demonstrates the complete RequestDownload / TransferData / RequestTransferExit (0x34/0x36/0x37)
pipeline with CRC-32 integrity, AES-128-CMAC SecurityAccess, and atomic A/B slot swap via MCUboot.

Included with **Developer** and **Professional** licenses.

---

## DFU sequence

```
UDS tester (TestLab / CANoe / scan tool)
    │
    │  0x10 0x02     DiagnosticSessionControl → programmingSession
    │  0x27 0x01     SecurityAccess → RequestSeed
    │  0x27 0x02     SecurityAccess → SendKey (AES-128-CMAC)
    │  0x31 0xFF00   RoutineControl → CheckProgrammingPreconditions
    │  0x34          RequestDownload (address=image-1, length=<file_size>)
    │  0x36 ×N       TransferData (256-byte blocks, CRC-32 accumulated)
    │  0x37          RequestTransferExit (CRC-32 finalised and validated)
    │  0x11 0x01     ECUReset → hardReset
    │
    ▼
MCUboot wakes → validates image in secondary slot
            → swaps primary ↔ secondary
            → boots new application
            → main() calls boot_write_img_confirmed() → permanent
```

If `boot_write_img_confirmed()` is never reached (crash, WDT, panic),
MCUboot rolls back to the previous image on the next reset — the intended
safety net for a broken update.

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

| Nucleo CN8 | AF | Signal | TJA1051 |
|---|---|---|---|
| PD0 | AF9 | FDCAN1_RX | TXD |
| PD1 | AF9 | FDCAN1_TX | RXD |
| 3V3 | — | VCC | VCC |
| GND | — | GND | GND |

Pin mapping is defined in `boards/nucleo_h743zi/nucleo_h743zi.overlay` — no changes needed.

Bus: 500 kbit/s, sample point 87.5%.

---

## Flash layout

The Nucleo-H743ZI2 has 2 MB internal flash. Partition map (`nucleo_h743zi.overlay`):

| Partition | DTS label | Base address | Size | Purpose |
|---|---|---|---|---|
| MCUboot | — | 0x08000000 | 64 KB | Bootloader (built separately) |
| Primary slot | `image-0` | 0x08010000 | 896 KB | Active application |
| Secondary slot | `image-1` | 0x080F0000 | 896 KB | OTA staging area |
| NVS | `diag_nvs` | 0x081D0000 | 192 KB | UDS DTC + calibration NVM |

---

## Prerequisites

- Zephyr SDK ≥ 0.16.8
- `west` workspace initialised from the EDS repository manifest
- MCUboot available as a west module (`west update` fetches it from `west.yml`)
- `imgtool`: `pip install imgtool`
- RSA-2048 signing key pair

Generate a development key (do **not** use in production):

```sh
imgtool keygen --key root-rsa-2048.pem --type rsa-2048
```

---

## Build

### 1. Build and flash MCUboot

```sh
west build -s bootloader/mcuboot/boot/zephyr \
           -b nucleo_h743zi \
           -d build-mcuboot \
           -- -DCONFIG_BOOT_SIGNATURE_TYPE_RSA=y \
              -DCONFIG_BOOT_SIGNATURE_KEY_FILE=\"root-rsa-2048.pem\"

west flash --build-dir build-mcuboot
```

### 2. Build the application

```sh
west build -b nucleo_h743zi examples/safeboot_ecu -d build-safeboot
```

### 3. Sign the image

```sh
west sign -t imgtool \
          --build-dir build-safeboot \
          -- --key root-rsa-2048.pem \
             --version 1.0.0+0
```

Signed binary: `build-safeboot/zephyr/zephyr.signed.bin`

### 4. Flash the initial application

```sh
west flash --build-dir build-safeboot
```

On first boot, `main()` calls `boot_write_img_confirmed()`. Serial output
(USART3, 115200 baud):

```
[OTA] New image confirmed — rollback guard cleared.
```

---

## Performing a DFU update

### With Xaloqi TestLab

```yaml
# campaigns/safeboot_dfu.yaml
target:
  transport: can
  interface: can0
  bitrate: 500000
  rx_id: 0x7DF
  tx_id: 0x7E8

campaigns:
  - name: OTA_DFU
    steps:
      - service: DiagnosticSessionControl
        args: { session: programming }
      - service: SecurityAccess
        args: { level: 1 }
      - service: RoutineControl
        args: { type: start, rid: 0xFF00 }
      - service: RequestDownload
        args:
          address: 0x080F0000
          length: !filesize build-safeboot/zephyr/zephyr.signed.bin
      - service: TransferData
        args:
          data: !file build-safeboot/zephyr/zephyr.signed.bin
          block_length: 256
      - service: RequestTransferExit
      - service: ECUReset
        args: { type: hardReset }
```

```sh
testlab run campaigns/safeboot_dfu.yaml
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
31 01 FF 00  →  71 01 FF 00 01 00      (0x01 0x00 = PASS)

# 5. RequestDownload  (ALFID 0x44 = 4-byte addr + 4-byte len)
34 00 44  08 0F 00 00  <len3> <len2> <len1> <len0>
→  74 20 01 00                          (maxBlockLen = 256)

# 6. TransferData — repeat for each block (blk_seq wraps 0x01–0xFF)
36 <blk_seq> <256 bytes>  →  76 <blk_seq>

# 7. RequestTransferExit
37  →  77

# 8. Hard reset — MCUboot swaps and boots new image
11 01  →  51 01  (then device resets)
```

---

## Post-DFU verification

Read DID 0xF181 after reboot to confirm the active image version:

```sh
testlab read-did --did 0xF181
```

Expected response for `v1.0.0`:

```
F181: 76 31 2E 30 2E 30 00 00   →  "v1.0.0\0\0"
```

Update `s_mock_applicationsoftwareidentification` in `generated/did_handlers.c`
before signing each new image so the version reflects the build.

---

## DIDs

| DID | Name | Size | Value |
|---|---|---|---|
| 0xF190 | VehicleIdentificationNumber | 17 B | `XALQ1EDS00SFBT001` |
| 0xF18C | ECUSerialNumber | 8 B | `SFB00001` |
| 0xF181 | ApplicationSoftwareIdentification | 8 B | `v1.0.0\0\0` |
| 0xF186 | ActiveDiagnosticSession | 1 B | `0x01` (default) |
| 0xF18A | SystemSupplierIdentifier | 10 B | `XALOQI    ` |

All DIDs readable in default session at security level 0.

---

## Routines

| RID | Name | Session | Security | Description |
|---|---|---|---|---|
| 0xFF00 | CheckProgrammingPreconditions | Extended | 0 | Verifies ECU is safe to enter programming mode |
| 0xFF01 | VerifyBootloaderIntegrity | Programming | 1 | Reads image_0 header and checks MCUboot magic |

### Result format (both routines)

| Byte | Meaning |
|---|---|
| 0 | `0x01` PASS · `0x02` FAIL |
| 1 | `0x00` none · `0x01` flash open err · `0x02` read err · `0x03` magic mismatch |

**VerifyBootloaderIntegrity** reads the first 4 bytes of flash area `image_0`
and checks for MCUboot image header magic `0x96f3b83d` (LE bytes: `3D B8 F3 96`).
A mismatch means the primary slot contains a corrupted or unsigned image.

---

## DTCs

| DTC | Trigger |
|---|---|
| 0xF00001 | CRC-32 mismatch on RequestTransferExit |
| 0xF00002 | Flash erase failure during RequestDownload |
| 0xF00003 | Flash write failure during TransferData |

---

## MCUboot image confirmation

`main()` calls `boot_is_img_confirmed()` during startup. On the first boot
after an OTA swap this returns `false`. `boot_write_img_confirmed()` is then
called to mark the image permanent. If the application fails to reach this
point before a reset, MCUboot reverts to the previous image automatically.

Requires `CONFIG_MCUBOOT_IMG_MANAGER=y` and `CONFIG_BOOTLOADER_MCUBOOT=y`
(already set in `boards/nucleo_h743zi/nucleo_h743zi.conf`).

---

## ASIL-B properties

- No dynamic memory allocation (`CONFIG_HEAP_MEM_POOL_SIZE=0`)
- No recursion in any code path
- Pre-start self-test in `uds_generated_init()` Step 1.1
- Safety violation counter in each generated service handler
- Static stack allocation (`K_THREAD_STACK_DEFINE`)
- Hardware watchdog fed every 1 ms in the diagnostics loop

---

## File structure

```
safeboot_ecu/
├── diagnostics_config.yaml        5 DIDs, 3 DTCs, safeboot.enabled: true
├── src/main.c                     MCUboot confirmation + diagnostics thread
├── CMakeLists.txt                 Includes zephyr_flash_ops.c from platform/
├── prj.conf                       Base Kconfig (CAN, logging, WDT, no heap)
├── boards/nucleo_h743zi/
│   ├── nucleo_h743zi.conf         Flash map + MCUboot Kconfig
│   └── nucleo_h743zi.overlay      FDCAN1, IWDG, RNG, USART3, flash partitions
└── generated/
    ├── uds_init.c                 Step 5.7 calls zephyr_flash_ops_init()
    ├── did_handlers.c             5 DID read handlers with example data
    └── routine_handlers.c         0xFF00 preconditions + 0xFF01 magic check
```

The `safeboot.enabled: true` flag in `diagnostics_config.yaml` causes codegen
to wire `zephyr_flash_ops_init()` into `generated/uds_init.c` automatically at
Step 5.7. No manual wiring required.

---

## See also

- [`examples/basic_ecu/`](../basic_ecu/) — minimal example without OTA (Community license, free)
- `platform/zephyr/zephyr_flash_ops.c` — flash erase/write/verify callbacks (image-1)
- `generated/uds_init.c` — full 9-step init sequence including flash ops wiring
- MCUboot documentation: <https://docs.mcuboot.com>
- EDS protocol reference: `docs/uds_services.md`
