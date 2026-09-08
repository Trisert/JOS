/* ---------------------------------------------------------------------------
 * fakes/main.h - minimal stand-in for Core/Inc/main.h in host unit tests.
 *
 * The flight main.h drags in the whole STM32L4 HAL, CMSIS and FreeRTOS tree,
 * none of which compiles (or means anything) on a host. This header declares
 * only the handful of HAL types, constants and prototypes that the modules
 * under test actually reference:
 *
 *     App/memory/memory.c -> HAL_SPI_Transmit/Receive, HAL_GPIO_WritePin,
 *                            HAL_FLASH_Program, HAL_FLASHEx_Erase,
 *                            HAL_FLASH_Unlock/Lock
 *     App/comms/comms.c   -> SPI_HandleTypeDef hspi1, NVIC_SystemReset()
 *     App/obsw/watchdog.c -> (nothing beyond the types below)
 *
 * The behaviour behind these prototypes lives in support/host_flash.c and
 * support/hal_stubs.c; the peripheral handle objects live in support/stubs.c.
 * Signatures are kept byte-compatible with the real HAL so that a file which
 * compiles here also compiles for the target.
 *
 * IMPORTANT (W2-6): this header is deliberately NOT mocked by CMock. Every
 * prototype below is *defined* by a file on the :support: path, and Ceedling
 * links the whole support set into every test executable, so a generated
 * mock_main.c would collide with those definitions at link time. Tests that
 * need to observe a reboot request use HOST_EXPECT_NVIC_RESET() from
 * support/host_support.h instead - a stronger seam than a CMock expectation
 * because it also reproduces "NVIC_SystemReset() does not return".
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

/* ---------- SPI (FM24VN10-G FRAM behind hspi2, SPF v3 3.6.4.2) ---------- */
typedef struct {
    uint32_t instance;   /* opaque on the host */
} SPI_HandleTypeDef;

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *hspi, const uint8_t *pData,
                                   uint16_t Size, uint32_t Timeout);
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *hspi, uint8_t *pData,
                                  uint16_t Size, uint32_t Timeout);

/* ---------- GPIO (FRAM software chip-selects) ----------
 * Opaque port objects: the code under test only passes them through to
 * HAL_GPIO_WritePin, whose double decodes (port, pin) into a chip index. */
typedef struct {
    uint32_t id;         /* opaque on the host */
} GPIO_TypeDef;

extern GPIO_TypeDef gpioa_obj;
extern GPIO_TypeDef gpiob_obj;
extern GPIO_TypeDef gpioc_obj;
#define GPIOA (&gpioa_obj)
#define GPIOB (&gpiob_obj)
#define GPIOC (&gpioc_obj)

#define GPIO_PIN_0   0x0001U
#define GPIO_PIN_1   0x0002U
#define GPIO_PIN_4   0x0010U
#define GPIO_PIN_13  0x2000U

typedef enum {
    GPIO_PIN_RESET = 0,
    GPIO_PIN_SET   = 1
} GPIO_PinState;

void HAL_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin);

/* FRAM chip-select mapping, mirrored from Core/Inc/main.h (single source of
 * truth for the flight build; duplicated here because the flight main.h drags
 * in the HAL and cannot compile on the host). */
#define FRAM_CS0_Pin         GPIO_PIN_4
#define FRAM_CS0_GPIO_Port   GPIOA
#define FRAM_CS1_Pin         GPIO_PIN_0
#define FRAM_CS1_GPIO_Port   GPIOB
#define FRAM_CS2_Pin         GPIO_PIN_1
#define FRAM_CS2_GPIO_Port   GPIOB
#define FRAM_CS3_Pin         GPIO_PIN_13
#define FRAM_CS3_GPIO_Port   GPIOC

/* ---------- Other peripheral handle types (W2-6) ----------
 * App/comms/comms.c declares `extern SPI_HandleTypeDef hspi1;` and other
 * App modules reference the ADC/IWDG handles. Only the handle *objects* are
 * needed on the host: every HAL entry point that would consume them is either
 * a support-file double or is never reached by a host test, so the structs
 * are deliberately opaque one-word placeholders rather than a copy of the
 * CubeMX layout (which would be a second, silently-diverging definition).
 * (SPI_HandleTypeDef itself is declared in the SPI section above.) */

typedef struct {
    uint32_t instance;
} ADC_HandleTypeDef;

typedef struct {
    uint32_t instance;
} IWDG_HandleTypeDef;

/* CubeMX peripheral handles - defined in support/stubs.c, except hspi2 which
 * lives in support/host_flash.c next to the FRAM double that observes it. */
extern SPI_HandleTypeDef  hspi2;   /* FM24VN10-G FRAM bus (SPI2) */
extern SPI_HandleTypeDef  hspi1;   /* SX1268 LoRa transceiver  */
extern ADC_HandleTypeDef  hadc1;   /* BMS measurements         */
extern IWDG_HandleTypeDef hiwdg;   /* independent watchdog     */

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

/* CubeMX error trap. Referenced by App/ code on some paths; defined in
 * support/hal_stubs.c so a host test that reaches it fails loudly instead of
 * spinning like the flight implementation does. */
void Error_Handler(void);

/* Error-flag clearing is a register write on target; a no-op on the host. */
#define __HAL_FLASH_CLEAR_FLAG(__FLAG__)  do { (void)(__FLAG__); } while (0)

#endif /* HOST_FAKE_MAIN_H */
