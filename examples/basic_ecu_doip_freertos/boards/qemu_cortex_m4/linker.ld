/*
 * linker.ld — QEMU mps2-an386 (ARM Cortex-M4)
 *
 * Memory map for QEMU mps2-an386, as modelled in QEMU's hw/arm/mps2.c:
 *   ZBT SSRAM1   : 0x00000000 – 0x003FFFFF (4 MB)  — code ("FLASH")
 *   ZBT SSRAM2&3 : 0x20000000 – 0x203FFFFF (4 MB)  — data
 *
 * SRAM is declared as 1 MB rather than the earlier 256 KB: the lwIP build
 * (-DLWIP_DIR=...) adds the lwIP heap, the pbuf pool and a larger FreeRTOS
 * heap to .bss. 1 MB stays comfortably inside the 4 MB the machine provides
 * while leaving obvious headroom.
 *
 * Xaloqi EDS basic_ecu_doip_freertos — CI/QEMU build only.
 * For production targets, replace with your MCU's linker script.
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 1M
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
        . = . + 0x2000;    /* 8 KB initial stack (MSP: startup + all ISRs) */
        . = ALIGN(8);
        _stack_end = .;
    } > SRAM

    _end = .;
}
