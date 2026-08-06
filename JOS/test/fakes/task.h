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

#endif /* JOS_TEST_FAKE_TASK_H */
