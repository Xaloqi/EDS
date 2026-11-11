/* MISRA analysis stub for FreeRTOS task.h (EDS#583). Declarations only. */
#ifndef MISRA_STUB_FREERTOS_TASK_H
#define MISRA_STUB_FREERTOS_TASK_H

#include "FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;

BaseType_t xTaskCreate(TaskFunction_t pxTaskCode, const char *pcName,
                       configSTACK_DEPTH_TYPE usStackDepth, void *pvParameters,
                       UBaseType_t uxPriority, TaskHandle_t *pxCreatedTask);
void vTaskDelay(TickType_t xTicksToDelay);
void vTaskSuspend(TaskHandle_t xTaskToSuspend);

#endif
