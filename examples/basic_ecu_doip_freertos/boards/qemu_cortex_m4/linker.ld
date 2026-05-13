/*
 * linker.ld — QEMU mps2-an386 (ARM Cortex-M4)
 *
 * Memory map for QEMU mps2-an386:
 *   FLASH: 0x00000000 – 0x003FFFFF (4 MB)
 *   SRAM:  0x20000000 – 0x20FFFFFF (16 MB addressable, 4 MB typical)
 *
 * Xaloqi EDS basic_ecu_freertos — CI build only.
 * For production targets, replace with your MCU's linker script.
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 256K
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
