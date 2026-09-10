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
  -nographic -monitor none \
  -serial file:boot.log
```

Expected contents of `boot.log`:

```
EDS basic_ecu_freertos: boot
EDS basic_ecu_freertos: starting scheduler
```

Both lines come from `src/main.c` over the board's UART0
(`examples/common/boards/qemu_cortex_m4/board_log.c`). Their absence means the
image is not running — check `arm-none-eabi-size build/eds_freertos.elf` first:
`.text` should be tens of kilobytes. A `.text` of a few hundred bytes, together
with a `cannot find entry symbol Reset_Handler` warning at link time, means the
board support in `examples/common/boards/qemu_cortex_m4/` was not compiled in,
and `--gc-sections` has deleted the program (EDS issue #268).

Piping QEMU's `-serial stdio` straight into another command can swallow this
output; `-serial file:` is the reliable form.

## Production integration

Replace `loopback_can_send()` in `src/main.c` with your MCU's CAN transmit
function. Wire your CAN RX interrupt to call `eds_platform_can_input(&frame)`.
Provide a `FreeRTOSConfig.h` appropriate for your MCU in `boards/<your_board>/`.

See `platform/platform_api.h` for the full integration contract.
