// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/safeboot_freertos_ecu/boards/nucleo_h743zi/startup.c
 *
 * PURPOSE: ARMv7-M reset vector and C runtime startup for the STM32H743ZI
 *          (NUCLEO-H743ZI2).
 *
 *          Like the three QEMU boards, this board declared
 *          ENTRY(Reset_Handler) in its linker script without defining either
 *          a vector table or a Reset_Handler, so --gc-sections discarded the
 *          whole program and the ELF came out empty (EDS issue #268).
 *
 *          This is deliberately NOT the shared
 *          examples/common/boards/qemu_cortex_m4/startup.c. That file is
 *          written for QEMU's mps2-an386: FLASH at 0x00000000 and 32 external
 *          IRQs. The STM32H743 differs in both respects, and a vector table
 *          that is too short is silently wrong — the NVIC would dispatch
 *          high-numbered interrupts into whatever .text follows the table.
 *
 *          Responsibilities, in order:
 *            1. Publish the vector table at the start of FLASH (.isr_vector).
 *            2. Point VTOR at it (see below).
 *            3. Copy .data from its FLASH load address into DTCM.
 *            4. Zero .bss.
 *            5. Call main().
 *
 * MEMORY MAP: taken from this board's own linker.ld, which matches the
 *          partition layout in the repository's Zephyr board files
 *          (boards/nucleo_h743zi/nucleo_h743zi.overlay: 2 MB dual-bank flash
 *          based at 0x08000000):
 *            FLASH bank 1 : 0x08000000, 1 MB   — running application
 *            DTCM         : 0x20000000, 128 KB — stack, .data, .bss
 *            AXI SRAM     : 0x24000000, 512 KB — FreeRTOS heap
 *
 * IRQ COUNT: 150 external interrupts (IRQ 0..149). Taken from ST's own CMSIS
 *          device header for this part (STMicroelectronics/cmsis_device_h7,
 *          Include/stm32h743xx.h), whose IRQn_Type enumeration ends at
 *          WAKEUP_PIN_IRQn = 149. The table is therefore 16 + 150 = 166
 *          entries, 664 bytes.
 *
 * VTOR   : the STM32H7 boot address is programmable (BOOT_ADD0/BOOT_ADD1
 *          option bytes) and a bootloader may already have moved VTOR, so
 *          Reset_Handler sets VTOR explicitly to this image's own table
 *          rather than assuming the reset default. The example's README
 *          describes the application running from bank 1 after a
 *          customer-supplied bootloader performs the bank swap, which is
 *          exactly the case where an inherited VTOR would be wrong.
 *
 * VERIFICATION: this board has no emulator in this repository and no hardware
 *          was available when it was written, so — unlike the three QEMU
 *          boards — it is verified only by construction: it links to a
 *          non-empty image whose vector table sits at 0x08000000 with a
 *          plausible initial MSP and reset vector. It has NOT been executed.
 *          See the pull request for issue #268 for the exact wording of what
 *          was and was not proven.
 *
 * SAFETY  : Example startup code — not safety-assessed.
 * STANDARD: MISRA C:2012 alignment intended. No malloc, no recursion.
 * =============================================================================
 */

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Symbols provided by linker.ld
 * ------------------------------------------------------------------------ */

extern uint32_t _sidata;      /**< .data load address in FLASH            */
extern uint32_t _sdata;       /**< .data start in DTCM                    */
extern uint32_t _edata;       /**< .data end in DTCM                      */
extern uint32_t _sbss;        /**< .bss start                             */
extern uint32_t _ebss;        /**< .bss end                               */
extern uint32_t _stack_end;   /**< top of the initial (MSP) stack         */

/* ---------------------------------------------------------------------------
 * FreeRTOS ARM_CM7 port handlers
 * ------------------------------------------------------------------------ */

extern void vPortSVCHandler(void);
extern void xPortPendSVHandler(void);
extern void xPortSysTickHandler(void);

extern int main(void);

void Reset_Handler(void);

/* ---------------------------------------------------------------------------
 * System control block — VTOR only
 *
 * Declared locally rather than pulling in CMSIS: this example does not vendor
 * the ST HAL or CMSIS headers, and one register does not justify the
 * dependency. SCB->VTOR is at 0xE000ED08 on every ARMv7-M part.
 * ------------------------------------------------------------------------ */

#define SCB_VTOR_ADDR   (0xE000ED08UL)
#define SCB_VTOR        (*(volatile uint32_t *)SCB_VTOR_ADDR)

/* ---------------------------------------------------------------------------
 * Default handlers
 *
 * Each spins rather than returning: an unexpected fault that returns simply
 * re-faults forever and hides the original cause. A halted CPU is observable
 * from a debugger; a re-entering fault loop is not.
 * ------------------------------------------------------------------------ */

static void Default_Handler(void)
{
    for (;;) {
        /* Intentionally empty: halt on unexpected exception. */
    }
}

void NMI_Handler(void)         __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)    __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)    __attribute__((weak, alias("Default_Handler")));
void IRQ_Default_Handler(void) __attribute__((weak, alias("Default_Handler")));

/* ---------------------------------------------------------------------------
 * Vector table
 * ------------------------------------------------------------------------ */

typedef void (*isr_fn_t)(void);

/*
 * Slot 0 is the initial MSP value, not a handler. Storing it as a function
 * pointer would convert an object pointer to a function pointer, which ISO C
 * forbids and MISRA C:2012 Rule 1.1 flags. The union keeps both kinds of
 * entry honestly typed, so no deviation record is needed.
 */
typedef union {
    isr_fn_t  handler;   /**< slots 1..N: exception/IRQ handler   */
    uintptr_t value;     /**< slot 0: initial stack pointer value */
} isr_entry_t;

/*
 * 150 external interrupts, IRQ 0..149 (WAKEUP_PIN_IRQn is the last, = 149 in
 * ST's stm32h743xx.h). Every external slot is filled with the halting default
 * handler: this example takes no peripheral interrupt of its own. A customer
 * wiring up FDCAN1, USART3 or the IWDG overrides the relevant entries by
 * defining a strong symbol, exactly as with ST's own startup file.
 */
#define STM32H743_NUM_IRQ  (150U)

const isr_entry_t g_isr_vector[16U + STM32H743_NUM_IRQ]
    __attribute__((section(".isr_vector"), used)) = {
    { .value   = (uintptr_t)&_stack_end },  /*  0: Initial MSP value   */
    { .handler = Reset_Handler       },  /*  1: Reset               */
    { .handler = NMI_Handler         },  /*  2: NMI                 */
    { .handler = HardFault_Handler   },  /*  3: HardFault           */
    { .handler = MemManage_Handler   },  /*  4: MemManage           */
    { .handler = BusFault_Handler    },  /*  5: BusFault            */
    { .handler = UsageFault_Handler  },  /*  6: UsageFault          */
    { .value   = 0U                  },  /*  7: Reserved            */
    { .value   = 0U                  },  /*  8: Reserved            */
    { .value   = 0U                  },  /*  9: Reserved            */
    { .value   = 0U                  },  /* 10: Reserved            */
    { .handler = vPortSVCHandler     },  /* 11: SVCall  — FreeRTOS  */
    { .handler = DebugMon_Handler    },  /* 12: DebugMon            */
    { .value   = 0U                  },  /* 13: Reserved            */
    { .handler = xPortPendSVHandler  },  /* 14: PendSV  — FreeRTOS  */
    { .handler = xPortSysTickHandler },  /* 15: SysTick — FreeRTOS  */

    /* External interrupts 0..149. */
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler },
    { .handler = IRQ_Default_Handler }, { .handler = IRQ_Default_Handler }
};

/* ---------------------------------------------------------------------------
 * Reset_Handler
 * ------------------------------------------------------------------------ */

void Reset_Handler(void)
{
    uint32_t       *dst = &_sdata;
    const uint32_t *src = &_sidata;

    /* 1. Take ownership of the vector table. A bootloader that jumped here
     *    may have left VTOR pointing at its own table. */
    SCB_VTOR = (uint32_t)(uintptr_t)&g_isr_vector[0];

    /* 2. Copy initialised data from FLASH to DTCM. */
    while (dst < &_edata) {
        *dst = *src;
        dst++;
        src++;
    }

    /* 3. Zero the BSS. */
    dst = &_sbss;
    while (dst < &_ebss) {
        *dst = 0U;
        dst++;
    }

    /* 4. Enter the application. main() does not return; the loop below is a
     *    belt-and-braces guard only. */
    (void)main();

    for (;;) {
        /* Intentionally empty. */
    }
}
