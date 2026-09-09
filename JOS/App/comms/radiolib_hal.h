#ifndef RADIOLIB_HAL_H
#define RADIOLIB_HAL_H

/*
 * radiohal.h — STM32 HAL adapter for RadioLib (SX1268 / LoRa1268F30)
 *
 * Ported from Marco-42/RedPill-T (satellite/stm32_lora/Core/Inc/STM32Hal.h),
 * adapted for JOS (Trisert/JOS, STM32L496VGTX).
 *
 * PROVENANCE / LICENSING NOTE (DO NOT IGNORE):
 *   RedPill-T is published on GitHub WITHOUT an explicit license ("license: null"
 *   on the repo metadata) => default "all rights reserved". Vendoring this adapter
 *   into a flight OBSW requires an explicit grant or license alignment with JOS.
 *   Tracked in redpill/jos-radiolib-plan (gbrain).
 *
 * PIN MAPPING — JOS uses a SEPARATE COMMS board (LoRa1268F30 module) wired to the
 * OBC through a 20-pin connector. Signal names are from RED_SPF_V3 (pag 90/95).
 * OBC V2.0 netlist assignments (canonical defines in Core/Inc/main.h):
 *   CS_TTC    = PA4 (SPI1 NSS, adjacent to PA5/6/7)
 *   LoRa_Busy = PC4 (plain input, no EXTI)
 *   GPIO_INT  = PB0 / EXTI0, rising (SX1268 DIO1: TX_DONE/RX_DONE)
 *   LoRa_NRST = PB1, shared with DEPLOY_SENSE (SPF pag 95).
 * PB6/PB7 are USART1_TX/RX and PD12/PD13 are I2C4_SCL/SDA per the netlist,
 * so the radio pins must stay off those lines.
 *
 * Special constraint (SPF pag 95): LoRa_NRST shares ONE OBC GPIO with DEPLOY_SENSE
 * (antenna deployment switch). The OBC must toggle that pin between input+pull-up
 * (read switch) and output (drive SX1268 reset). See configureResetPin()/releaseResetPin().
 */

#include "main.h"            /* stm32l4xx_hal.h, hspi1, GPIO defs        */
#include <RadioLib.h>

/* RadioLib virtual pin IDs (mapped to CubeMX pins in radiohal.cpp). */
#define RLIB_NSS   0
#define RLIB_RESET 1
#define RLIB_DIO1  2
#define RLIB_BUSY  3

/*
 * PLACEHOLDER GPIO bindings — replace with the real OBC-schematic assignments.
 * Naming follows the SPF COMMS signal names so the mapping is self-documenting.
 *   CS_TTC     -> SX1268 NSS      (COMMS conn pin 14)
 *   LoRa_Busy  -> SX1268 BUSY     (COMMS conn pin 13)
 *   GPIO_INT   -> SX1268 DIO1/IRQ (COMMS conn pin 15, route to EXTI)
 * LoRa_NRST is NOT defined here: it is LoRa_NRST_GPIO_Port/LoRa_NRST_Pin from
 * main.h (PB1, MULTIPLEXED w/ DEPLOY_SENSE, COMMS conn pin 4).
 */
#define CS_TTC_GPIO_Port    GPIOA          /* SPI1 NSS, OBC V2.0 netlist */
#define CS_TTC_Pin          GPIO_PIN_4
#define LoRa_Busy_GPIO_Port GPIOC          /* SX1268 BUSY -> PC4 */
#define LoRa_Busy_Pin       GPIO_PIN_4
#define GPIO_INT_GPIO_Port  GPIOB          /* SX1268 DIO1 -> PB0 / EXTI0 */
#define GPIO_INT_Pin        GPIO_PIN_0

/* SPI instance used for the radio (SPF pag 77: OBC<->SX1268 on SPI1). */
extern SPI_HandleTypeDef hspi1;

struct Stm32Pin {
    GPIO_TypeDef* port;
    uint16_t      pin;
};

class STM32Hal : public RadioLibHal {
public:
    /* RadioLibHal ctor requires (input, output, low, high, rising, falling)
       interrupt-mode constants. We handle DIO1 via HAL_GPIO_EXTI_Callback, so
       we pass the "disabled" constants (RadioLib uses them only if you call
       attachInterrupt, which we leave empty). */
    explicit STM32Hal(SPI_HandleTypeDef* spiHandle);

    void addPin(uint32_t pinId, GPIO_TypeDef* port, uint16_t pin);

    // GPIO
    void pinMode(uint32_t pin, uint32_t mode) override;
    void digitalWrite(uint32_t pin, uint32_t value) override;
    uint32_t digitalRead(uint32_t pin) override;

    // SPI
    void spiBegin() override;
    void spiBeginTransaction() override;
    void spiTransfer(uint8_t* out, size_t len, uint8_t* in) override;
    void spiEndTransaction() override;
    void spiEnd() override;

    // Time
    void delay(RadioLibTime_t ms) override;
    void delayMicroseconds(RadioLibTime_t us) override;
    unsigned long millis() override;
    unsigned long micros() override;

    // Interrupts — left empty; DIO1 is handled via HAL_GPIO_EXTI_Callback in comms.
    void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) override {}
    void detachInterrupt(uint32_t interruptNum) override {}
    void yield() override {}

    long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override;

    /*
     * Multiplexed reset/deploy pin (SPF pag 95).
     * The same OBC GPIO drives LoRa_NRST (output, active low) and reads
     * DEPLOY_SENSE (input, pull-up). Never both at once.
     */
    void configureResetPin(void);   /* set as output, idle HIGH (not reset) */
    void pulseReset(void);          /* active-low reset pulse for SX1268     */
    void releaseResetPin(void);     /* leave as output HIGH; call before use  */

private:
    SPI_HandleTypeDef* _spi;
    static const int   MAX_PINS = 8;
    Stm32Pin           _pinMap[MAX_PINS];
    Stm32Pin* getStmPin(uint32_t pinId);
};

#endif /* RADIOLIB_HAL_H */
