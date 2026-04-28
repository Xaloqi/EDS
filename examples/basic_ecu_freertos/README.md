# Xaloqi EDS — basic_ecu_freertos

FreeRTOS port of the BasicECU example. Identical diagnostic configuration to
`examples/basic_ecu/` (Zephyr) — same `diagnostics_config.yaml`, same codegen,
same 14 UDS services. Only the platform layer differs.

## What this proves

- The FreeRTOS platform HAL compiles and links cleanly
- `uds_generated_init()` works identically on FreeRTOS
- The same YAML configuration generates working firmware on both platforms
- ISO-TP + UDS stack runs in a FreeRTOS task at 1ms poll rate

## Quick start

```bash
# 1. Clone FreeRTOS kernel
git clone --depth=1 https://github.com/FreeRTOS/FreeRTOS-Kernel.git /opt/freertos-kernel

# 2. Generate code
python3 ../../tools/codegen.py \
  --config diagnostics_config.yaml \
  --out generated/ \
  --safety-wrappers --asil-level B --no-manifest

# 3. Build (QEMU Cortex-M4)
cmake -B build \
  -DEDS_PLATFORM=freertos \
  -DFREERTOS_DIR=/opt/freertos-kernel \
  -DBOARD=qemu_cortex_m4 \
  -GNinja .
ninja -C build

# 4. Run in QEMU (optional)
qemu-system-arm \
  -machine mps2-an386 \
  -cpu cortex-m4 \
  -kernel build/eds_freertos.elf \
  -nographic \
  -semihosting
```

## Production integration

Replace `loopback_can_send()` in `src/main.c` with your MCU's CAN transmit
function. Wire your CAN RX interrupt to call `eds_platform_can_input(&frame)`.
Provide a `FreeRTOSConfig.h` appropriate for your MCU in `boards/<your_board>/`.

See `platform/platform_api.h` for the full integration contract.
