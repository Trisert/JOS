#include "bms.h"
#include "main.h"   /* SPI_HandleTypeDef, HAL SPI API, CubeMX peripheral handles */

/* Stub BMS — returns fixed values until real subsystem SPI driver is ready.
 * Per RED_DES_ElectronicArchitecture_V1:
 *   BQ76905 is on the EPS board, connected to the EPS MCU via local I2C.
 *   The OBC queries the EPS over the subsystem SPI bus for battery telemetry
 *   (voltage, current, temperature, SoC, SoH).
 */

/* ---------- Subsystem SPI master to the EPS STM32L496 ----------
 * The OBC is the master on the subsystem bus; the EPS STM32L496 is the slave
 * that fronts the BQ76905. That bus is SPI2 (PB13 SCK / PB14 MISO /
 * PB15 MOSI): the handle lives in Core/Src/main.c, the peripheral clock and
 * the AF5 pin mux are set up by HAL_SPI_MspInit() in
 * Core/Src/stm32l4xx_hal_msp.c. The BMS binds to that handle instead of
 * declaring a second SPI_HandleTypeDef on the same peripheral — two handles
 * on one instance would each hold their own lock/state and corrupt each
 * other's transfers.
 */
extern SPI_HandleTypeDef hspi2;

/* Master handle used for every EPS transaction. NULL while the link is down,
   so a caller can tell "EPS never answered" from "EPS reported these
   values". */
static SPI_HandleTypeDef *bms_spi = NULL;

/* Link timing. SystemClock_Config() runs the PLL from MSI at 80 MHz with
   APB1 undivided, so SPI2 is clocked from an 80 MHz PCLK1 and /32 gives a
   2.5 MHz SCK. That is deliberately slower than the 10 MHz CubeMX default:
   the EPS link leaves the OBC board over the inter-board harness, where edge
   rates matter more than throughput, and the telemetry frames are a handful
   of bytes. Mode 0 (CPOL = low, CPHA = 1st edge), 8-bit, MSB first, matching
   the EPS slave configuration. NSS is software-driven: the EPS chip select is
   a plain GPIO, not the SPI2 NSS output. */
#define BMS_EPS_SPI_PRESCALER   SPI_BAUDRATEPRESCALER_32

/* Optional EPS chip-select pin. The SATPF harness assignment for the EPS CS
   is not in the tree yet, so no pin is claimed by default — define both
   macros at build time (-DBMS_EPS_CS_PORT=GPIOx -DBMS_EPS_CS_PIN=GPIO_PIN_n)
   once the pinout is fixed and the line is configured here, idle high. The
   GPIOA/B/C/E clocks are already enabled by MX_GPIO_Init(). */
#if defined(BMS_EPS_CS_PORT) && defined(BMS_EPS_CS_PIN)
static void bms_eps_cs_init(void)
{
    GPIO_InitTypeDef cs = {0};

    /* Park the line inactive before it becomes an output, so no glancing low
       pulse is presented to the EPS while the pin mode is switched. */
    HAL_GPIO_WritePin(BMS_EPS_CS_PORT, BMS_EPS_CS_PIN, GPIO_PIN_SET);

    cs.Pin   = BMS_EPS_CS_PIN;
    cs.Mode  = GPIO_MODE_OUTPUT_PP;
    cs.Pull  = GPIO_NOPULL;
    cs.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BMS_EPS_CS_PORT, &cs);
}
#endif

/* Bring up (or re-apply) the subsystem SPI master configuration for the EPS
   link and bind it to the BMS. HAL_SPI_Init() only runs MspInit when the
   handle is still in HAL_SPI_STATE_RESET, so calling it here after
   MX_SPI2_Init() re-applies the register configuration without touching the
   clocks or the pin mux, and calling it before MX_SPI2_Init() (or after a
   fault de-initialised the bus) still brings the peripheral up.

   Any other master-side device that later shares SPI2 must re-apply its own
   Init parameters before its transactions — the bus configuration is
   per-transfer state, not per-device.

   Returns 0 when the master is ready for EPS traffic, -1 otherwise. */
static int bms_spi_init(void)
{
    bms_spi = NULL;

    hspi2.Instance               = SPI2;
    hspi2.Init.Mode              = SPI_MODE_MASTER;
    hspi2.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi2.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi2.Init.CLKPolarity       = SPI_POLARITY_LOW;    /* SPI mode 0 */
    hspi2.Init.CLKPhase          = SPI_PHASE_1EDGE;
    hspi2.Init.NSS               = SPI_NSS_SOFT;
    hspi2.Init.BaudRatePrescaler = BMS_EPS_SPI_PRESCALER;
    hspi2.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi2.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi2.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi2.Init.CRCPolynomial     = 7;
    hspi2.Init.CRCLength         = SPI_CRC_LENGTH_DATASIZE;
    hspi2.Init.NSSPMode          = SPI_NSS_PULSE_DISABLE; /* software NSS */

    if (HAL_SPI_Init(&hspi2) != HAL_OK) {
        return -1;
    }

    if (HAL_SPI_GetState(&hspi2) != HAL_SPI_STATE_READY) {
        return -1;
    }

#if defined(BMS_EPS_CS_PORT) && defined(BMS_EPS_CS_PIN)
    bms_eps_cs_init();
#endif

    bms_spi = &hspi2;
    return 0;
}

static bms_status_t bms_status = {
    .soc        = 100,
    .temp_c     = 250,
    .voltage_mv = 7400,
};

void bms_init(void)
{
    /* Bring up the subsystem SPI master to the EPS STM32L496 (SPI slave).
       A failure is not fatal at boot: bms_spi stays NULL, bms_spi_ready()
       reports the link as down and bms_get_status() keeps serving the
       conservative defaults instead of garbage telemetry. The state machine
       must never be blocked from booting by a silent EPS. */
    (void)bms_spi_init();
}

bool bms_spi_ready(void)
{
    return (bms_spi != NULL);
}

bms_status_t bms_get_status(void)
{
    /* TODO: query EPS MCU over subsystem SPI for BQ76905 telemetry —
       assert the EPS chip select, HAL_SPI_TransmitReceive() the telemetry
       request on *bms_spi, release the chip select, validate the reply and
       update bms_status. Until that transaction exists the cached defaults
       are returned; bms_spi_ready() tells callers which of the two they are
       looking at. */
    return bms_status;
}
