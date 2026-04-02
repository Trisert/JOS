#include "clear.h"
#include "memory.h"
#include "main.h"
#include "stm32l4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

extern ADC_HandleTypeDef hadc1;

static clear_status_t status;

/* Burst buffer — 5400 × 8 B ≈ 42 KB.
 * Too large for stack; placed in BSS.
 * TODO: for flight, stream directly to FRAM instead of buffering. */
static clear_burst_point_t burst_buf[CLEAR_BURST_MAX];
static uint16_t burst_idx;

/* ---------- Internal helpers ---------- */

static uint16_t adc_read_mv(uint32_t channel)
{
    ADC_ChannelConfTypeDef sConfig = {0};
    sConfig.Channel      = channel;
    sConfig.Rank         = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = ADC_SAMPLETIME_247CYCLES_5;
    sConfig.SingleDiff   = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset       = 0;

    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
        return 0;

    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 10) != HAL_OK)
        return 0;

    uint32_t raw = HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);
    return (uint16_t)((raw * 3000) / 4095);
}

/* ---------- Public API ---------- */

void clear_init(void)
{
    memset(&status, 0, sizeof(status));
    burst_idx = 0;

    /* All LEDs off */
    HAL_GPIO_WritePin(CLEAR_LED_FZ_A_PORT,  CLEAR_LED_FZ_A_PIN,  GPIO_PIN_RESET);
    HAL_GPIO_WritePin(CLEAR_LED_FZ_B_PORT,  CLEAR_LED_FZ_B_PIN,  GPIO_PIN_RESET);
    HAL_GPIO_WritePin(CLEAR_LED_FmZ_A_PORT, CLEAR_LED_FmZ_A_PIN, GPIO_PIN_RESET);
}

void clear_led_set(clear_face_t face, clear_led_colour_t colour)
{
    if (face >= CLEAR_FACE_COUNT) return;

    status.led_colour[face] = (uint8_t)colour;
    status.led_on[face] = 1;

    if (face == CLEAR_FACE_Z) {
        HAL_GPIO_WritePin(CLEAR_LED_FZ_A_PORT, CLEAR_LED_FZ_A_PIN,
                          (colour == CLEAR_LED_WHITE) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(CLEAR_LED_FZ_B_PORT, CLEAR_LED_FZ_B_PIN,
                          (colour == CLEAR_LED_IR)   ? GPIO_PIN_SET : GPIO_PIN_RESET);
    } else {
        HAL_GPIO_WritePin(CLEAR_LED_FmZ_A_PORT, CLEAR_LED_FmZ_A_PIN,
                          (colour != CLEAR_LED_OFF) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
}

void clear_led_off(clear_face_t face)
{
    if (face >= CLEAR_FACE_COUNT) return;

    status.led_on[face] = 0;
    status.led_colour[face] = CLEAR_LED_OFF;

    if (face == CLEAR_FACE_Z) {
        HAL_GPIO_WritePin(CLEAR_LED_FZ_A_PORT, CLEAR_LED_FZ_A_PIN, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(CLEAR_LED_FZ_B_PORT, CLEAR_LED_FZ_B_PIN, GPIO_PIN_RESET);
    } else {
        HAL_GPIO_WritePin(CLEAR_LED_FmZ_A_PORT, CLEAR_LED_FmZ_A_PIN, GPIO_PIN_RESET);
    }
}

uint16_t clear_read_pd(clear_face_t face)
{
    if (face >= CLEAR_FACE_COUNT) return 0;
    uint32_t ch = (face == CLEAR_FACE_Z) ? CLEAR_PD_FZ_CH : CLEAR_PD_FmZ_CH;
    uint16_t mv = adc_read_mv(ch);
    status.pd_mv[face] = mv;
    return mv;
}

void clear_burst_start(void)
{
    burst_idx = 0;
    status.burst_active = 1;
    status.burst_count = 0;
}

void clear_burst_stop(void)
{
    if (burst_idx > 0) {
        /* Write burst data to FRAM via cyclic buffer */
        cyclic_buffer_write((const uint8_t *)burst_buf,
                            burst_idx * sizeof(clear_burst_point_t));
    }
    status.burst_active = 0;
    status.burst_count = burst_idx;
}

clear_status_t clear_get_status(void)
{
    return status;
}

/* ---------- CLEAR FreeRTOS task ---------- */

/* Sample at 1 Hz during burst */
#define CLEAR_BURST_INTERVAL_MS  1000

static void clear_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (status.burst_active && burst_idx < CLEAR_BURST_MAX) {
            burst_buf[burst_idx].timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            burst_buf[burst_idx].pd_fz_mv   = clear_read_pd(CLEAR_FACE_Z);
            burst_buf[burst_idx].pd_fmz_mv  = clear_read_pd(CLEAR_FACE_MZ);
            burst_idx++;
            status.burst_count = burst_idx;

            if (burst_idx >= CLEAR_BURST_MAX) {
                clear_burst_stop();
            }
        }

        osDelay(pdMS_TO_TICKS(CLEAR_BURST_INTERVAL_MS));
    }
}

osThreadId_t clear_task_create(void)
{
    static const osThreadAttr_t attrs = {
        .name       = "clear",
        .stack_size = 256 * 4,
        .priority   = osPriorityBelowNormal,
    };
    return osThreadNew(clear_task, NULL, &attrs);
}
