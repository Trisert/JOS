/**
 * @file    fakes/stm32l4xx_hal.h
 * @brief   Host-test stand-in for the STM32L4 HAL.
 *
 * Declares only the HAL surface JOS/App actually calls. CMock generates
 * mock_stm32l4xx_hal.c from this header, which lets the tests simulate Flash
 * programming/erase, FRAM I2C transfers and the independent watchdog without
 * any silicon.
 *
 * Standards: ECSS-E-ST-40C §5.5 (unit test with simulated environment),
 * JPL Rule 16 (check the return value of every non-void function — mirrored
 * here by making every HAL entry point return HAL_StatusTypeDef).
 */
#ifndef JOS_TEST_FAKE_STM32L4XX_HAL_H
#define JOS_TEST_FAKE_STM32L4XX_HAL_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    HAL_OK      = 0x00,
    HAL_ERROR   = 0x01,
    HAL_BUSY    = 0x02,
    HAL_TIMEOUT = 0x03
} HAL_StatusTypeDef;

/* ---------- Peripheral handles (opaque for host tests) ---------- */
typedef struct { uint32_t instance; } I2C_HandleTypeDef;
typedef struct { uint32_t instance; } SPI_HandleTypeDef;
typedef struct { uint32_t instance; } ADC_HandleTypeDef;
typedef struct { uint32_t instance; } IWDG_HandleTypeDef;
typedef struct { uint32_t instance; } GPIO_TypeDef;

/* ---------- GPIO ---------- */
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;

#define GPIO_PIN_0    ((uint16_t)0x0001)
#define GPIO_PIN_1    ((uint16_t)0x0002)
#define GPIO_PIN_2    ((uint16_t)0x0004)
#define GPIO_PIN_4    ((uint16_t)0x0010)
#define GPIO_PIN_10   ((uint16_t)0x0400)
#define GPIO_PIN_12   ((uint16_t)0x1000)
#define GPIO_PIN_14   ((uint16_t)0x4000)

extern GPIO_TypeDef *const GPIOB;
extern GPIO_TypeDef *const GPIOC;
extern GPIO_TypeDef *const GPIOE;

void HAL_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState);

/* ---------- Core timing ---------- */
uint32_t HAL_GetTick(void);
void     HAL_Delay(uint32_t Delay);

/* ---------- I2C (FM24VN10-G FRAM) ---------- */
#define I2C_MEMADD_SIZE_8BIT   ((uint32_t)0x00000001U)
#define I2C_MEMADD_SIZE_16BIT  ((uint32_t)0x00000010U)

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                   uint16_t MemAddress, uint16_t MemAddSize,
                                   uint8_t *pData, uint16_t Size, uint32_t Timeout);
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                    uint16_t MemAddress, uint16_t MemAddSize,
                                    uint8_t *pData, uint16_t Size, uint32_t Timeout);

/* ---------- Internal Flash (LastStates pool) ---------- */
#define FLASH_TYPEPROGRAM_DOUBLEWORD  ((uint32_t)0x00U)
#define FLASH_TYPEERASE_PAGES         ((uint32_t)0x00U)
#define FLASH_BANK_1                  ((uint32_t)0x01U)
#define FLASH_FLAG_ALL_ERRORS         ((uint32_t)0x0000C3FBU)

/* Flag clearing is a register write on target; a no-op on the host. */
#define __HAL_FLASH_CLEAR_FLAG(__FLAG__)  ((void)(__FLAG__))

typedef struct {
    uint32_t TypeErase;
    uint32_t Banks;
    uint32_t Page;
    uint32_t NbPages;
} FLASH_EraseInitTypeDef;

HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address, uint64_t Data);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError);

/* ---------- ADC (BMS) ---------- */
HAL_StatusTypeDef HAL_ADC_Start(ADC_HandleTypeDef *hadc);
HAL_StatusTypeDef HAL_ADC_Stop(ADC_HandleTypeDef *hadc);
HAL_StatusTypeDef HAL_ADC_PollForConversion(ADC_HandleTypeDef *hadc, uint32_t Timeout);
uint32_t          HAL_ADC_GetValue(ADC_HandleTypeDef *hadc);

/* ---------- Independent watchdog (IWDG) ----------
 * The hardware IWDG is not yet enabled in the flight build (see the TODO in
 * App/obsw/state_machine.c); the seam is mocked here so the refresh call can
 * be verified the moment CubeMX generates hiwdg. */
HAL_StatusTypeDef HAL_IWDG_Refresh(IWDG_HandleTypeDef *hiwdg);

#endif /* JOS_TEST_FAKE_STM32L4XX_HAL_H */
