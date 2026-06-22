/*
 * linker.ld — STM32H743ZI (Nucleo-H743ZI2)
 *
 * Xaloqi EDS safeboot_freertos_ecu — OTA DFU example
 *
 * Flash layout (2 MB dual-bank):
 *   Bank 1: 0x08000000 – 0x080FFFFF  1 MB  (8 sectors × 128 KB) — running app
 *   Bank 2: 0x08100000 – 0x081DFFFF  896 KB (7 sectors × 128 KB) — OTA staging
 *   NVM   : 0x081E0000 – 0x081FFFFF  128 KB (sector 7 Bank 2) — UDS NVM
 *
 * RAM layout:
 *   DTCM  : 0x20000000 – 0x2001FFFF  128 KB — fastest, used for stack + FreeRTOS
 *   AXI   : 0x24000000 – 0x2407FFFF  512 KB — heap, static buffers
 *
 * Application runs from Bank 1. OTA image is written to Bank 2 via 0x34/0x36/0x37.
 * Bank swap at boot is handled by the customer's bootloader.
 */

MEMORY
{
    FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K  /* Bank 1 — running app */
    OTA    (r)   : ORIGIN = 0x08100000, LENGTH = 896K   /* Bank 2 — OTA staging (not linked, just documented) */
    NVM    (rw)  : ORIGIN = 0x081E0000, LENGTH = 128K   /* Last sector of Bank 2 */
    DTCM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K   /* D1 DTCM — stack + TCBs */
    SRAM   (rw)  : ORIGIN = 0x24000000, LENGTH = 512K   /* D1 AXI SRAM — heap + buffers */
}

ENTRY(Reset_Handler)

SECTIONS
{
    /* Interrupt vector table — must be at start of FLASH */
    .isr_vector :
    {
        . = ALIGN(512);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    /* Code and read-only data */
    .text :
    {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
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

    /* Initialised data — load from FLASH, copy to DTCM at startup */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > DTCM AT > FLASH

    /* Zero-initialised data */
    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > DTCM

    /* FreeRTOS heap — in AXI SRAM for size */
    .heap (NOLOAD) :
    {
        . = ALIGN(8);
        _heap_start = .;
        . = . + (64K);
        . = ALIGN(8);
        _heap_end = .;
    } > SRAM

    /* Main stack in DTCM — 8 KB */
    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        _stack_start = .;
        . = . + 0x2000;
        . = ALIGN(8);
        _stack_end = .;
    } > DTCM

    _end = .;
}
