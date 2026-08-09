/**
 * @file    fakes/FreeRTOS.h
 * @brief   Host-test stand-in for the FreeRTOS kernel configuration header.
 *
 * Only the types and macros referenced by JOS/App are provided. No functions
 * are declared here on purpose: everything callable lives in fakes/task.h so
 * that CMock produces a single, focused mock_task.c.
 */
#ifndef JOS_TEST_FAKE_FREERTOS_H
#define JOS_TEST_FAKE_FREERTOS_H

#include <stdint.h>
#include <stddef.h>

typedef uint32_t TickType_t;
typedef long     BaseType_t;
typedef unsigned long UBaseType_t;

#define pdTRUE                  ((BaseType_t)1)
#define pdFALSE                 ((BaseType_t)0)
#define pdPASS                  pdTRUE
#define pdFAIL                  pdFALSE

/* 1 kHz tick, matching FreeRTOSConfig.h (configTICK_RATE_HZ = 1000). */
#define configTICK_RATE_HZ      1000U
#define portTICK_PERIOD_MS      ((TickType_t)(1000U / configTICK_RATE_HZ))
#define pdMS_TO_TICKS(xMs)      ((TickType_t)(((TickType_t)(xMs) * configTICK_RATE_HZ) / 1000U))
#define portMAX_DELAY           ((TickType_t)0xFFFFFFFFU)

#endif /* JOS_TEST_FAKE_FREERTOS_H */
