# ISO-TP Transmit Frame Padding

## Overview

EDS supports optional padding of ISO-TP TX frames via `ISOTP_TX_PADDING`.
When enabled, unused bytes in SF, FF (CAN FD escape), CF, and FC frames are
filled with `ISOTP_TX_PADDING_BYTE` (default `0xCC`) and the DLC is extended
to the next full frame size.

ISO 15765-2 Annex B permits but does not mandate TX padding. Receivers MUST
ignore padding bytes per the standard.

## When to enable

Enable when interoperating with OEM diagnostic testers or bus analysers that
require fixed-length frames — for example, Vector CANalyzer configurations or
production EOL testers.

## Configuration

### Zephyr — `prj.conf`

```
CONFIG_ISOTP_TX_PADDING=y
CONFIG_ISOTP_TX_PADDING_BYTE=0xCC
```

### FreeRTOS / bare-metal — compiler flag

```
-DISOTP_TX_PADDING=1
-DISOTP_TX_PADDING_BYTE=0xCC
```

Or before `#include "isotp.h"`:

```c
#define ISOTP_TX_PADDING      1
#define ISOTP_TX_PADDING_BYTE 0xCCU
```

## Frame behaviour

| Frame type       | Padding off             | Padding on                        |
|------------------|-------------------------|-----------------------------------|
| Classic CAN SF   | DLC = length + 1        | DLC = 8, tail filled with 0xCC    |
| CAN FD SF        | DLC = length + 2        | DLC = next valid FD DLC (≥ 12)    |
| Classic CAN FF   | DLC = 8 (always full)   | DLC = 8 (unchanged — no padding needed) |
| CAN FD FF escape | DLC = 6 + first\_data   | DLC = next valid FD DLC           |
| Classic CAN CF   | DLC = bytes\_in\_cf + 1 | DLC = 8, tail filled with 0xCC    |
| FC               | DLC = 3                 | DLC = 8, bytes [3..7] = 0xCC      |

CAN FD DLC valid values above 8: 12, 16, 20, 24, 32, 48, 64.
Unused bytes are rounded up to the next value in this sequence.

## Standard reference

ISO 15765-2:2023 §8 and Annex B — TX padding is permitted; receivers must
treat padding bytes as undefined and ignore them.

## Implementation note

The feature is guarded entirely by `#if ISOTP_TX_PADDING`. When the flag is 0
(the default), no padding code is compiled into the binary and all frame DLCs
are set to the exact number of bytes used. Classic CAN FF is always 8 bytes
regardless of this flag because the FF encoding fills all 8 bytes by definition.

## Interop note: padding asymmetry with xaloqi-tester

EDS pads with `0xCC` (when padding is enabled). The xaloqi-tester Python
client (`xaloqi/tester/_isotp.py`) pads outgoing frames with `0x00`.

Per ISO 15765-2 §8, receivers **must** treat padding bytes as undefined and
ignore them, so this asymmetry is not a correctness issue. However, it is
visible in bus capture tools (CANalyzer, Wireshark, CANoe) as a difference
in the padding byte between tester-to-ECU and ECU-to-tester frames. If a
third-party tester logs an alert for inconsistent padding, this is the cause —
not a protocol violation.
