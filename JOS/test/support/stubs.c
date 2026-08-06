/* Test support: symbols the flight build gets from the linker script or from
 * CubeMX-generated code, provided here so the host test executables link.
 *
 * Ceedling compiles and links everything under :paths: :support: into every
 * test executable.
 *
 * Standards: ECSS-E-ST-40C §5.5 (unit testing in a simulated environment).
 */
#include <stdint.h>
#include "main.h"          /* test/fakes/main.h — fake HAL handle types */

/* ---------- Linker-provided firmware image bounds (boot_crc.c) ----------
 * In the real firmware these come from STM32L496VGTX_FLASH.ld; in the host
 * test they are plain arrays so boot_crc_verify() links and can be exercised.
 * Placed in .data so they have a real address the linker can resolve. */
uint8_t __fw_image_start[1024] = {0};
uint8_t __fw_crc_start[4] = {0};

/* ---------- CubeMX peripheral handles ----------
 * App/ modules reference these as externs. The tests never touch the contents:
 * every HAL entry point that consumes them is a CMock mock. */
I2C_HandleTypeDef  hi2c2;    /* FM24VN10-G FRAM bus     */
SPI_HandleTypeDef  hspi1;    /* SX1268 LoRa transceiver */
ADC_HandleTypeDef  hadc1;    /* BMS measurements        */
IWDG_HandleTypeDef hiwdg;    /* independent watchdog    */

/* ---------- GPIO port bases ----------
 * Real addresses on target (AHB2 peripheral map); dummy objects on the host. */
static GPIO_TypeDef gpiob_obj, gpioc_obj, gpioe_obj;
GPIO_TypeDef *const GPIOB = &gpiob_obj;
GPIO_TypeDef *const GPIOC = &gpioc_obj;
GPIO_TypeDef *const GPIOE = &gpioe_obj;
