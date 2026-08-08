/* ---------------------------------------------------------------------------
 * fakes/main.h - minimal stand-in for Core/Inc/main.h in host unit tests.
 *
 * The flight main.h drags in the whole STM32L4 HAL, CMSIS and FreeRTOS tree,
 * none of which compiles (or means anything) on a host. This header declares
 * only the handful of HAL types, constants and prototypes that the modules
 * under test actually reference:
 *
 *     App/memory/memory.c -> HAL_I2C_Mem_Read/Write, HAL_FLASH_Program,
 *                            HAL_FLASHEx_Erase, HAL_FLASH_Unlock/Lock
 *
 * The behaviour behind these prototypes lives in support/host_flash.c.
 * Signatures are kept byte-compatible with the real HAL so that a file which
 * compiles here also compiles for the target.
 * ------------------------------------------------------------------------- */
#ifndef HOST_FAKE_MAIN_H
#define HOST_FAKE_MAIN_H

#include <stdint.h>
#include <stddef.h>

/* ---------- HAL core ---------- */
typedef enum {
    HAL_OK      = 0x00,
    HAL_ERROR   = 0x01,
    HAL_BUSY    = 0x02,
    HAL_TIMEOUT = 0x03
} HAL_StatusTypeDef;

/* ---------- I2C (FM24VN10-G FRAM behind hi2c2) ---------- */
typedef struct {
    uint32_t instance;   /* opaque on the host */
} I2C_HandleTypeDef;

#define I2C_MEMADD_SIZE_8BIT   0x00000001U
#define I2C_MEMADD_SIZE_16BIT  0x00000010U

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                   uint16_t MemAddress, uint16_t MemAddSize,
                                   uint8_t *pData, uint16_t Size, uint32_t Timeout);

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                    uint16_t MemAddress, uint16_t MemAddSize,
                                    uint8_t *pData, uint16_t Size, uint32_t Timeout);

/* ---------- Internal Flash ---------- */
#define FLASH_TYPEPROGRAM_DOUBLEWORD  0x00000000U
#define FLASH_TYPEERASE_PAGES         0x00000000U
#define FLASH_BANK_1                  0x00000001U
#define FLASH_BANK_2                  0x00000002U
#define FLASH_FLAG_EOP                0x00000001U
#define FLASH_FLAG_ALL_ERRORS         0x0000C3FBU

/* STM32L4 bank page size, needed by memory.c's compile-time pool guards. */
#define FLASH_PAGE_SIZE               2048U

/* __HAL_FLASH_GET_FLAG reads a status register on target; on host it returns
 * 0 (no flags pending) so the polling loops in memory.c fall through. */
#define __HAL_FLASH_GET_FLAG(__FLAG__)  (0U)

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

/* Host test doubles for CMSIS/HAL symbols used by the modules under test.
 * On target these live in cmsis_gcc.h / stm32l4xx_hal.c; here they are
 * explicit, documented no-ops so the firmware compiles on the x86-64 runner.
 * (NASA-STD-8739.8: test doubles must be explicit, never silent.) */
uint32_t HAL_GetTick(void);
#define __DSB()  do { __asm__ volatile ("" ::: "memory"); } while (0)
#define __ISB()  do { __asm__ volatile ("" ::: "memory"); } while (0)
void    NVIC_SystemReset(void);

/* Error-flag clearing is a register write on target; a no-op on the host. */
#define __HAL_FLASH_CLEAR_FLAG(__FLAG__)  do { (void)(__FLAG__); } while (0)

#endif /* HOST_FAKE_MAIN_H */
