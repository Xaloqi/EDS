# Shared example board support

Board-level code shared by the FreeRTOS examples: the pieces that have to
exist for an image to boot at all, but that carry no diagnostics logic.

```
common/boards/
├── board_log.h              Boot-log interface every board implements
└── qemu_cortex_m4/
    ├── startup.c            ARMv7-M vector table + C runtime startup
    ├── freertos_hooks.c     vApplicationGetIdleTaskMemory()
    └── board_log.c          Polled CMSDK UART0 driver
```

Used by `basic_ecu_freertos`, `sensor_ecu_freertos` and
`safeboot_freertos_ecu` when built with `-DBOARD=qemu_cortex_m4`.

## Why this exists

Every one of those examples declared `ENTRY(Reset_Handler)` in its linker
script while defining neither a vector table nor a `Reset_Handler`. The
linker warned:

```
cannot find entry symbol Reset_Handler; defaulting to 00000000
```

and, with no root to trace from, `-Wl,--gc-sections` discarded the entire
program. The images linked cleanly and contained no `main()`. Compile-only CI
legs reported success against them for as long as they existed. See EDS issue
#268, and #267 for the same defect in `basic_ecu_doip_freertos`.

## Why shared rather than copied

All three `qemu_cortex_m4` board directories specify identical FreeRTOS
settings — the `ARM_CM4F` port, a 25 MHz `configCPU_CLOCK_HZ`,
`configSUPPORT_STATIC_ALLOCATION = 1`, `configUSE_TIMERS = 0` — and target
the same emulated machine. Three copies of one vector table would drift, and
the repository already prefers centralising this kind of thing
(`cmake/eds_service_sources.cmake`, `cmake/eds_build_mode.cmake`).

`basic_ecu_doip_freertos` keeps its own local copy under
`boards/qemu_cortex_m4/` and is not built from this directory. It is the one
FreeRTOS example with a proven end-to-end DoIP campaign behind it, and moving
it without being able to re-run that campaign would risk a regression for a
tidiness gain.

## Boards that are not QEMU

`board_log.h` is the only thing here a non-QEMU board is expected to
implement. `safeboot_freertos_ecu`'s `nucleo_h743zi` board supplies its own
`startup.c`, `freertos_hooks.c` and `board_log.c`, because the STM32H743's
flash base, interrupt count and FreeRTOS configuration all differ from the
emulated machine's. A vector table that is too short is silently wrong, so
nothing here is reused across genuinely different parts.

## For your own hardware

This directory is CI scaffolding, not a starting point to copy. On a real MCU
you will use your vendor's startup file and linker script. What is worth
lifting is the shape: the FreeRTOS port's `vPortSVCHandler`,
`xPortPendSVHandler` and `xPortSysTickHandler` must occupy vector slots 11,
14 and 15, and `vApplicationGetIdleTaskMemory()` must exist whenever
`configSUPPORT_STATIC_ALLOCATION` is 1.
