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
| STM32 Nucleo-H743ZI2 **or** Nucleo-H753ZI | Target board |
| 3.3 V CAN transceiver breakout (TJA1051T/3, SN65HVD230, …) | CAN physical layer |
| USB-CAN adapter (PEAK PCAN-USB, Kvaser Leaf, CANable, etc.) | Host-side CAN |
| 2× 120 Ω resistors | CAN bus termination |

> The transceiver must be a **bare** transceiver — `TXD`/`RXD` logic pins
> plus `CANH`/`CANL`. Boards that bundle a *CAN controller* (e.g. an MCP2515
> behind SPI) will not work: this example drives the STM32's own FDCAN1
> peripheral, which needs direct access to the transceiver's logic pins.

### Wiring (FDCAN1)

| Nucleo CN9 | Zio pin | AF | MCU signal | Transceiver |
|---|---|---|---|---|
| D66 | 27 | AF9 | PD1 / FDCAN1_TX | **TXD** |
| D67 | 25 | AF9 | PD0 / FDCAN1_RX | **RXD** |
| 3V3 | — | — | — | VCC |
| GND | — | — | — | GND |

Both signals connect **straight across, not crossed**: on a CAN transceiver
`TXD` is an *input* driven by the controller's transmit pin and `RXD` is an
*output* feeding the controller's receive pin — the names are already written
from the controller's point of view. (Connector and pin numbers per ST UM1974,
Table 20, "CN9 Zio connector pinout".)

Pin mapping is defined in the board's `.overlay` under `boards/` — no changes
needed.

Bus: 500 kbit/s, sample point 87.5%. Terminate **both** ends of the bus with
120 Ω across CANH/CANL — many USB-CAN adapters have a switchable or
solder-jumper terminator on board, in which case only one discrete resistor is
needed at the transceiver end.

---

## Flash layout

Both boards have 2 MB internal flash in two banks of 8 × 128 KB erase sectors.
Partition map, as defined by the board `.overlay` — **the overlay is the source
of truth; this table documents it** (#278):

| Partition | DTS label | Base address | Size | Sectors | Purpose |
|---|---|---|---|---|---|
| MCUboot | `boot_partition` | 0x08000000 | 256 KB | 2 | Bootloader (built separately) |
| Primary slot | `image-0` | 0x08040000 | 768 KB | 6 | Active application |
| Secondary slot | `image-1` | 0x08100000 | 768 KB | 6 | OTA staging area |
| NVS | `diag_nvs` | 0x081C0000 | 256 KB | 2 | UDS DTC + calibration NVM |

Three constraints fix these numbers, and all three are easy to get wrong:

1. **Every boundary lands on a 128 KB erase-sector edge.** The hardware erases
   in whole 128 KB sectors, so a partition that starts or ends mid-sector lets
   an erase of one region corrupt the tail of its neighbour.
2. **`image-0` and `image-1` must be the same size** — MCUboot swaps between
   them.
3. **MCUboot needs its own partition.** It links against whatever
   `zephyr,code-partition` resolves to; with no `boot_partition` node it falls
   back to `slot0_partition` and is linked at the same address as the
   application image.

Those three together are why the slots are 768 KB rather than a rounder-looking
896 KB: 2 MB minus 2 sectors for MCUboot and 2 for NVS leaves 12 sectors, split
evenly between two equal slots.

> **RequestDownload's `memoryAddress` is flash-DEVICE-relative, not the
> Cortex-M memory-mapped address.** `platform/zephyr/zephyr_flash_ops.c`
> populates its memory-map region straight from Zephyr's `flash_area` API
> (`fa->fa_off`), which reports offsets relative to the flash controller —
> the same convention `west sign`'s own "partition offset" printout uses
> (`0x40000` for `image-0`, not `0x08040000`). `image-1`'s `0x08100000` in
> the table above is therefore sent over UDS as **`0x00100000`**, confirmed
> by reading `s_flash_region` live via GDB on real hardware — anything else
> is refused with NRC 0x31 (requestOutOfRange). The examples below use the
> correct relative value throughout.

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

Set `BOARD` to whichever board you have; everything below is identical
otherwise.

```sh
BOARD=nucleo_h753zi          # or nucleo_h743zi
WS=$PWD                      # west workspace topdir (contains zephyr/, bootloader/)
EDS=$WS/EDS                  # path to this repository inside the west workspace
KEY=$WS/root-rsa-2048.pem
```

> **Both builds must be given the board overlay and conf explicitly.** This
> repository keeps them at `examples/safeboot_ecu/boards/<board>/` rather than
> a path Zephyr auto-detects, so a plain `west build -b <board>` silently omits
> them and fails with `DIAG_CAN_DEV undeclared` (no `can0` alias) or an
> undefined `eds_nvs_slot`. This is what `ci.yml` passes too.

### 1. Build and flash MCUboot

MCUboot gets the board **overlay** — it has to agree with the application about
where the partitions are — plus its **own** `app.overlay`, which is what
repoints `zephyr,code-partition` at `boot_partition` so MCUboot links at the
start of flash instead of on top of `image-0`. `-DDTC_OVERLAY_FILE` *replaces*
Zephyr's default overlay list rather than adding to it, so `app.overlay` has to
be named explicitly or it is silently dropped.

> **Do not pass the application's `.conf` to the bootloader.** MCUboot is a
> separate application; the board `.conf` here is the *app's* Kconfig fragment.
> Feeding it to MCUboot pulls in `CONFIG_WATCHDOG=y` / `CONFIG_IWDG_STM32=y`,
> and since `CONFIG_WDT_DISABLE_AT_BOOT` is unset, Zephyr then arms a 100 ms
> independent watchdog *inside the bootloader*. MCUboot does not feed it, so on
> an image large enough that signature verification runs past the window, the
> watchdog resets the board before the application is ever entered — an
> endless reboot loop in which MCUboot logs `Swap type: none` and nothing else
> ever starts.

```sh
west build --pristine -b $BOARD -d build-mcuboot \
           -s $WS/bootloader/mcuboot/boot/zephyr \
           -- -DCONFIG_BOOT_SIGNATURE_TYPE_RSA=y \
              "-DCONFIG_BOOT_SIGNATURE_KEY_FILE=\"$KEY\"" \
              "-DDTC_OVERLAY_FILE=$EDS/examples/safeboot_ecu/boards/$BOARD/$BOARD.overlay;$WS/bootloader/mcuboot/boot/zephyr/app.overlay"

west flash --build-dir build-mcuboot
```

Confirm MCUboot targeted its own partition before moving on:

```sh
grep -E 'CONFIG_FLASH_LOAD_(OFFSET|SIZE)' build-mcuboot/zephyr/.config
# CONFIG_FLASH_LOAD_OFFSET=0x0
# CONFIG_FLASH_LOAD_SIZE=0x40000
```

If `OFFSET` matches `image-0`'s base instead, `app.overlay` was dropped and
MCUboot would be linked on top of the application.

### 2. Build the application

```sh
west build --pristine -b $BOARD -d build-safeboot \
           -s $EDS/examples/safeboot_ecu \
           -- -DDIAG_SKIP_CODEGEN=ON \
              "-DEXTRA_CONF_FILE=$EDS/examples/safeboot_ecu/boards/$BOARD/$BOARD.conf" \
              "-DDTC_OVERLAY_FILE=$EDS/examples/safeboot_ecu/boards/$BOARD/$BOARD.overlay"
```

`-DDIAG_SKIP_CODEGEN=ON` builds against the pre-committed
`examples/safeboot_ecu/generated/` sources. Drop it only if you hold a
Developer or Professional licence — regenerating requires `tools/templates/`,
which is not part of a public checkout.

### 3. Sign the image

```sh
west sign -t imgtool \
          --build-dir build-safeboot \
          -- --key $KEY \
             --version 1.0.0+0
```

Signed binary: `build-safeboot/zephyr/zephyr.signed.bin`

`west sign` prints the slot it targeted — check it matches `image-0` above:

```
partition offset: 262144 (0x40000)
partition size:   786432 (0xc0000)
```

### 4. Flash the initial application

**Flash the signed image explicitly.** `west flash` selects
`zephyr.hex` — the *unsigned* build product — which has no MCUboot image
header, so MCUboot will reject it and refuse to boot the application. Point
the runner at `zephyr.signed.hex` instead:

```sh
west flash --build-dir build-safeboot --hex-file build-safeboot/zephyr/zephyr.signed.hex
```

Or, with openocd directly (`verify` is worth having here):

```sh
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
        -c "program build-safeboot/zephyr/zephyr.signed.hex verify reset exit"
```

To tell the two apart: a signed image begins with the MCUboot magic
`3D B8 F3 96`, an unsigned one begins with the vector table (typically zeros
in the first record).

On first boot, `main()` calls `boot_write_img_confirmed()`. Serial output
(USART3, 115200 baud via the on-board ST-LINK VCP — `/dev/ttyACM0` on Linux):

```
*** Booting MCUboot v2.1.0 ***
<inf> mcuboot: Image index: 0, Swap type: none
*** Booting Zephyr OS build v3.7.0 ***
<inf> safeboot_ecu: Xaloqi EDS  v1.0.0
<inf> zephyr_wdt: WDT: Armed with 100 ms window (channel 0).
<inf> safeboot_ecu: UDS stack ready.
<inf> safeboot_ecu: Diagnostics task started (stack: 4096 bytes, priority: 5).
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
          address: 0x00100000  # image-1, flash-device-relative — see note above
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

Byte values below are a real capture from a NUCLEO-H753ZI (`fix/277-312-313-dfu-swap`)
except where shown symbolic (`<...>`) because they depend on the image being sent.

```
# 1. Programming session
10 02  →  50 02 00 32 01 F4              (P2max=50ms, P2*max=5000ms)

# 2. RequestSeed — seed is 8 bytes (UDS_ALGO_SEED_LEN, core/uds_security_algo.h)
27 01  →  67 01 <8-byte seed>

# 3. SendKey — first 4 bytes of AES-128-CMAC(level-1 key, seed)
27 02 <4-byte key>  →  67 02

# 4. CheckProgrammingPreconditions (optional — not enforced by 0x34 itself)
31 01 FF 00  →  71 01 FF 00 01 00      (0x01 0x00 = PASS)

# 5. RequestDownload (ALFID 0x44 = 4-byte addr + 4-byte len)
#    Address is flash-device-relative — see the callout above the flash
#    layout table. This ECU's erase_step_cb (issue #312) means 0x34 answers
#    immediately with NRC 0x78 while the secondary slot erases in the
#    background, one 128 KB sector per poll-loop iteration — expect one or
#    more 0x78 frames (one per completed sector) before the real [0x74].
34 00 44  00 10 00 00  <len3> <len2> <len1> <len0>
→  7F 34 78                              (responsePending — repeats per sector)
→  74 20 01 01                          (maxNumberOfBlockLength = 257)

# 6. TransferData — repeat for each block (blk_seq wraps 0x01→0xFF→0x01,
#    0x00 is always invalid). maxNumberOfBlockLength above INCLUDES the
#    1-byte block counter, so the payload here is 255 bytes, not 256.
#    Needs multi-frame ISO-TP for any block over 7 bytes — this ECU
#    advertises block_size=4 / STmin=1ms in its Flow Control (issue #313);
#    a sender that ignores that and blasts frames unpaced will overflow the
#    8-frame CAN RX queue.
36 <blk_seq> <up to 255 bytes>  →  76 <blk_seq>

# 7. RequestTransferExit — the CRC-32 record is MANDATORY here once any
#    image policy is registered without REQUIRE_PARAM_RECORD, which is
#    this ECU's default (platform/zephyr/zephyr_mcuboot_image_policy.c,
#    issue #277) — a bare [0x37] gets NRC 0x13. CRC-32 (poly 0xEDB88320,
#    init/final XOR 0xFFFFFFFF — the same algorithm Python's zlib.crc32()
#    implements) over every TransferData payload byte sent, in order.
37 <crc3> <crc2> <crc1> <crc0>  →  77

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
