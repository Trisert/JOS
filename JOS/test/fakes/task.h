/**
 * @file    fakes/task.h
 * @brief   Host-test stand-in for the FreeRTOS task API.
 *
 * Mocked by CMock (mock_task.c) so that the tick source used by
 * App/obsw/watchdog.c is fully controllable from the test.
 */
#ifndef JOS_TEST_FAKE_TASK_H
#define JOS_TEST_FAKE_TASK_H

#include "FreeRTOS.h"

typedef void *TaskHandle_t;

TickType_t   xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void         vTaskDelay(TickType_t xTicksToDelay);

/* Stack high-water mark in words left unused (INCLUDE_uxTaskGetStackHighWaterMark
   = 1 in Core/Inc/FreeRTOSConfig.h). Mocked so the watchdog monitor's per-scan
   HWM sampling (App/obsw/watchdog.c) is observable from the host suite.
   UBaseType_t comes from FreeRTOS.h above. */
UBaseType_t  uxTaskGetStackHighWaterMark(TaskHandle_t xTask);

#endif /* JOS_TEST_FAKE_TASK_H */
