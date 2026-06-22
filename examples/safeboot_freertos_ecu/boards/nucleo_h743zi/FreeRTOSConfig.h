/*
 * FreeRTOSConfig.h — STM32H743ZI (Nucleo-H743ZI2)
 *
 * Xaloqi EDS safeboot_freertos_ecu — OTA DFU example
 *
 * Key choices for EDS OTA on H743:
 *   configTICK_RATE_HZ = 1000 → 1 ms ticks (ISO-TP requires ms resolution)
 *   configCPU_CLOCK_HZ = 400 MHz (H743 max, adjust for your PLL config)
 *   configSUPPORT_STATIC_ALLOCATION = 1 → no heap for tasks/queues (MISRA)
 *   TOTAL_HEAP_SIZE = 64 KB — sufficient for UDS + ISO-TP + FreeRTOS internals
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ── Scheduler ── */
#define configUSE_PREEMPTION                    1
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCPU_CLOCK_HZ                      400000000UL  /* STM32H743 @ 400 MHz */
#define configTICK_RATE_HZ                      1000U         /* 1 ms tick — required for ISO-TP */
#define configMAX_PRIORITIES                    8U
#define configMINIMAL_STACK_SIZE                256U
#define configTOTAL_HEAP_SIZE                   (64U * 1024U)
#define configMAX_TASK_NAME_LEN                 16U
#define configUSE_TRACE_FACILITY                0
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   1U

/* ── Memory allocation ── */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        1

/* ── Mutexes and semaphores ── */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_QUEUE_SETS                    0

/* ── Software timers ── */
#define configUSE_TIMERS                        0

/* ── Hooks ── */
#define configCHECK_FOR_STACK_OVERFLOW          2   /* Trap stack overflows */
#define configUSE_MALLOC_FAILED_HOOK            1

/* ── Cortex-M7 interrupt priorities ──
 * STM32H743 uses 4-bit priority grouping (16 levels).
 * All ISRs that call FromISR FreeRTOS APIs must have numerical priority
 * >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY.
 */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15U
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5U
#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8U - 4U))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8U - 4U))

/* ── API inclusions ── */
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_xTaskGetSchedulerState          1

/* ── Cortex-M7 port specifics ── */
#define configENABLE_FPU                        1
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0

/* ── Assert ── */
#define configASSERT(x) \
    do { if (!(x)) { taskDISABLE_INTERRUPTS(); for (;;) { } } } while (0)

#endif /* FREERTOS_CONFIG_H */
