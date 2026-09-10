/*
 * linker.ld — QEMU mps2-an386 (ARM Cortex-M4)
 *
 * Memory map for QEMU mps2-an386, as modelled in QEMU's hw/arm/mps2.c:
 *   ZBT SSRAM1   : 0x00000000 – 0x003FFFFF (4 MB)  — code ("FLASH")
 *   ZBT SSRAM2&3 : 0x20000000 – 0x203FFFFF (4 MB)  — data
 *
 * SRAM is declared as the full 4 MB the machine provides rather than the
 * earlier 256 KB. This example's QEMU build backs the OTA staging slot with
 * a RAM stub (s_stub_flash in platform/freertos/freertos_flash_ops.c), which
 * alone is 896 KB — the size of the image-1 partition it stands in for. At
 * 256 KB the link fails outright:
 *   "section `.bss' will not fit in region `SRAM'; region overflowed by
 *    725696 bytes"
 * That was never observed before because this example had no vector table,
 * so --gc-sections deleted the whole program and .bss was empty (EDS #268).
 *
 * Xaloqi EDS safeboot_freertos_ecu — CI/QEMU build only.
 * For production targets, replace with your MCU's linker script — see
 * boards/nucleo_h743zi/linker.ld for the STM32H743 layout.
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}

ENTRY(Reset_Handler)

SECTIONS
{
    /* Vector table + text */
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* Init/fini arrays */
    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH

    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH

    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > FLASH

    /* Load address of data in FLASH, runtime address in SRAM */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT > FLASH

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > SRAM

    /* Stack at top of SRAM */
    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        _stack_start = .;
        . = . + 0x1000;    /* 4 KB initial stack */
        . = ALIGN(8);
        _stack_end = .;
    } > SRAM

    _end = .;
}
