#include "crystals.h"
#include "memory.h"
#include "main.h"
#include "stm32l4xx_hal.h"
#include <string.h>

extern ADC_HandleTypeDef hadc1;

static crystals_status_t status;

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
    /* Vref = 3.0 V, 12-bit ADC */
    return (uint16_t)((raw * 3000) / 4095);
}

/* ---------- Public API ---------- */

void crystals_init(void)
{
    memset(&status, 0, sizeof(status));

    /* All outputs start LOW (off) */
    HAL_GPIO_WritePin(CRYSTALS_EN_PORT,     CRYSTALS_EN_PIN,     GPIO_PIN_RESET);
    HAL_GPIO_WritePin(CRYSTALS_HEATER_PORT,  CRYSTALS_HEATER_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(CRYSTALS_CAM_TRIG_PORT, CRYSTALS_CAM_TRIG_PIN, GPIO_PIN_SET);
}

void crystals_enable(uint8_t on)
{
    HAL_GPIO_WritePin(CRYSTALS_EN_PORT, CRYSTALS_EN_PIN,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    status.enabled = on;
}

uint8_t crystals_is_enabled(void)
{
    return status.enabled;
}

void crystals_manage_heater(int16_t temp_c)
{
    if (temp_c <= CRYSTALS_HEATER_ON_TEMP && !status.heater_on) {
        HAL_GPIO_WritePin(CRYSTALS_HEATER_PORT, CRYSTALS_HEATER_PIN, GPIO_PIN_SET);
        status.heater_on = 1;
    } else if (temp_c >= CRYSTALS_HEATER_OFF_TEMP && status.heater_on) {
        HAL_GPIO_WritePin(CRYSTALS_HEATER_PORT, CRYSTALS_HEATER_PIN, GPIO_PIN_RESET);
        status.heater_on = 0;
    }
}

void crystals_take_photo(void)
{
    /* Pulse camera trigger: active-low, 100 ms */
    HAL_GPIO_WritePin(CRYSTALS_CAM_TRIG_PORT, CRYSTALS_CAM_TRIG_PIN, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(CRYSTALS_CAM_TRIG_PORT, CRYSTALS_CAM_TRIG_PIN, GPIO_PIN_SET);
}

uint8_t crystals_read_obscuration(void)
{
    status.pd_front_mv = adc_read_mv(CRYSTALS_PD_FRONT_CH);
    status.pd_rear_mv  = adc_read_mv(CRYSTALS_PD_REAR_CH);

    if (status.pd_front_mv == 0) {
        status.obscuration = 0;
        return 0;
    }

    /* Obscuration = 100% × (1 - V_rear / V_front) */
    int32_t pct = 100 - ((int32_t)status.pd_rear_mv * 100) / (int32_t)status.pd_front_mv;
    if (pct < 0)   pct = 0;
    if (pct > 100)  pct = 100;

    status.obscuration = (uint8_t)pct;
    return status.obscuration;
}

crystals_status_t crystals_get_status(void)
{
    return status;
}
