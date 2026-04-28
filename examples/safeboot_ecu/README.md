# SafeBoot ECU Example — Xaloqi EDS

**Secure OTA Firmware Update — UDS DFU (0x34 / 0x36 / 0x37) + MCUboot**

This example demonstrates **production-grade secure firmware update** over UDS using
the RequestDownload / TransferData / RequestTransferExit service sequence, with
MCUboot as the bootloader and CRC-32 integrity verification.

The reference for any ECU that requires secure OTA updates validated by a scan tool,
CI pipeline, or field service tool.

---

## What this example shows

```
UDS Tester (pytest / CANoe / scan tool)
    │
    │  0x10 0x02  DiagnosticSessionControl → programmingSession
    │  0x27 0x01  SecurityAccess → RequestSeed
    │  0x27 0x02  SecurityAccess → SendKey (AES-128-CMAC)
    │  0x34       RequestDownload (address, length, encryption method)
    │  0x36 ×N    TransferData (128-byte blocks with CRC accumulation)
    │  0x37       RequestTransferExit (CRC-32 validation)
    │  0x11 0x01  ECUReset → MCUboot validates + swaps image
    │
    ▼
MCUboot secondary slot → validates signature → swaps to primary
```

**Security:** Programming session requires SecurityAccess level 1 (AES-128-CMAC).  
**Integrity:** CRC-32 accumulator checked on RequestTransferExit.  
**Rollback:** MCUboot confirms new image before marking permanent.

---

## What this example contains

```
safeboot_ecu/
├── diagnostics_config.yaml      5 DIDs, 3 DTCs, safeboot.enabled: true
├── src/main.c                   Application + DFU state machine
├── CMakeLists.txt               MCUboot + secondary slot configuration
├── prj.conf                     CONFIG_BOOTLOADER_MCUBOOT=y
├── boards/native_sim/
└── generated/                   Pre-built codegen output with flash_ops wired
```

The `safeboot.enabled: true` flag in the YAML causes codegen to wire
`zephyr_flash_ops_init()` into the generated `uds_init.c` automatically.

---

## Availability

Included with **Developer** and **Professional** licenses.

**[Purchase at xaloqi.com →](https://xaloqi.com)**

---

See also: [`examples/basic_ecu/`](../basic_ecu/) — free community example.
