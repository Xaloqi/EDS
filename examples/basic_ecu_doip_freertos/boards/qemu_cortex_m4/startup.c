// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/basic_ecu_doip_freertos/boards/qemu_cortex_m4/startup.c
 *
 * PURPOSE: ARMv7-M reset vector and C runtime startup for QEMU mps2-an386.
 *
 *          Before this file existed the example had no vector table and no
 *          Reset_Handler at all: the linker emitted
 *            "cannot find entry symbol Reset_Handler; defaulting to 00000000"
 *          and, because nothing anchored a root for --gc-sections, the whole
 *          program was garbage-collected. The resulting ELF had .text size 0
 *          — an empty binary that could never boot. See EDS issue for the
 *          identical gap still present in basic_ecu_freertos.
 *
 *          Responsibilities, in order:
 *            1. Publish the ARMv7-M vector table at address 0 (.isr_vector),
 *               with the initial stack pointer and reset vector QEMU reads
 *               from offsets 0x0 and 0x4 at machine reset.
 *            2. Copy .data from its FLASH load address into SRAM.
 *            3. Zero .bss.
 *            4. Call main().
 *
 *          The SVC / PendSV / SysTick slots are wired directly to the
 *          FreeRTOS ARM_CM4F port entry points (vPortSVCHandler,
 *          xPortPendSVHandler, xPortSysTickHandler). Without these three the
 *          scheduler starts but never context-switches.
 *
 * TARGET : QEMU mps2-an386 (ARM Cortex-M4F). num-irq = 32 on this machine
 *          (hw/arm/mps2.c), so the table carries 32 external IRQ slots.
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
extern uint32_t _sdata;       /**< .data start in SRAM                    */
extern uint32_t _edata;       /**< .data end in SRAM                      */
extern uint32_t _sbss;        /**< .bss start                             */
extern uint32_t _ebss;        /**< .bss end                               */
extern uint32_t _stack_end;   /**< top of the initial (MSP) stack         */

/* ---------------------------------------------------------------------------
 * FreeRTOS ARM_CM4F port handlers
 * ------------------------------------------------------------------------ */

extern void vPortSVCHandler(void);
extern void xPortPendSVHandler(void);
extern void xPortSysTickHandler(void);

extern int main(void);

void Reset_Handler(void);

/* ---------------------------------------------------------------------------
 * Default handlers
 *
 * Each spins rather than returning: on a Cortex-M an unexpected fault that
 * returns simply re-faults forever and hides the original cause. A halted
 * CPU is observable in QEMU (-d int, or the monitor) whereas a fault loop
 * that keeps re-entering is not.
 * ------------------------------------------------------------------------ */

static void Default_Handler(void)
{
    for (;;) {
        /* Intentionally empty: halt on unexpected exception. */
    }
}

void NMI_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void IRQ_Default_Handler(void) __attribute__((weak, alias("Default_Handler")));

/* ---------------------------------------------------------------------------
 * Vector table
 *
 * KEEP(*(.isr_vector)) in linker.ld anchors this array, which in turn
 * references Reset_Handler -> main() -> the rest of the program. This is what
 * stops --gc-sections from discarding the entire image.
 * ------------------------------------------------------------------------ */

typedef void (*isr_fn_t)(void);

/*
 * Vector-table entries are a union rather than a bare function pointer.
 *
 * Slot 0 is not a handler at all — it is the initial MSP value the core
 * loads at reset. Storing it as a function pointer would require converting
 * an object pointer to a function pointer, which ISO C forbids and MISRA
 * C:2012 Rule 1.1 flags (confirmed by misra_analysis.py: "ISO C forbids
 * conversion of object pointer to function pointer type"). The union
 * expresses the two genuinely different kinds of entry directly, so no
 * deviation record is needed.
 */
typedef union {
    isr_fn_t  handler;   /**< slots 1..N: exception/IRQ handler   */
    uintptr_t value;     /**< slot 0: initial stack pointer value */
} isr_entry_t;

/* 16 ARMv7-M system vectors + 32 external IRQs (mps2-an386 num-irq = 32). */
#define MPS2_AN386_NUM_IRQ  (32U)

const isr_entry_t g_isr_vector[16U + MPS2_AN386_NUM_IRQ]
    __attribute__((section(".isr_vector"), used)) = {
    { .value = (uintptr_t)&_stack_end },   /*  0: Initial MSP value */
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

    /* External interrupts 0..31. The LAN9118 Ethernet controller is IRQ 13
     * on this machine, but the ethernetif driver polls its RX FIFO from a
     * FreeRTOS task rather than taking interrupts, so no slot is wired. */
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

    /* 1. Copy initialised data from FLASH to SRAM. */
    while (dst < &_edata) {
        *dst = *src;
        dst++;
        src++;
    }

    /* 2. Zero the BSS. */
    dst = &_sbss;
    while (dst < &_ebss) {
        *dst = 0U;
        dst++;
    }

    /* 3. Enter the application. main() does not return; the loop below is a
     *    belt-and-braces guard only. */
    (void)main();

    for (;;) {
        /* Intentionally empty. */
    }
}
