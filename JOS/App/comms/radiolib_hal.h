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
 * OBC through a 20-pin connector. Signal names are from RED_SPF_V3 (pag 90/95);
 * the antenna-side connector pinout is DEFINITIVE (TTC_SCH_BoardAntenna.pdf):
 * COMMS pin 3 DEPLOY_CMD, 4 DEPLOY_SENSE, 5 LoRa_NRST, 7 SPI_CLK, 8 SPI_MISO,
 * 9 SPI_MOSI, 13 LoRa_BUSY, 14 CS_TTC, 15 GPIO_INT/DIO1.
 *
 * OBC-side MCU assignments are SOFTWARE-DEFINED by this ICD (no OBC schematic
 * available): see docs/ICD_OBC_COMMS.md for the full table + rationale.
 * LoRa_NRST and DEPLOY_SENSE are SEPARATE OBC GPIOs (the old SPF pag 95
 * multiplexing is NOT used: the antenna board exposes them on distinct
 * connector pins 5 and 4). The deploy firing SEQUENCE itself is out of scope
 * here (owned by T1.7); this contract covers pins + init levels only.
 */

#include "main.h"            /* stm32l4xx_hal.h, hspi1, GPIO defs        */
#include <RadioLib.h>

/* RadioLib virtual pin IDs (mapped to CubeMX pins in radiohal.cpp). */
#define RLIB_NSS   0
#define RLIB_RESET 1
#define RLIB_DIO1  2
#define RLIB_BUSY  3

/*
 * OBC<->COMMS ICD pin bindings (software-defined, see docs/ICD_OBC_COMMS.md).
 *   CS_TTC     -> SX1268 NSS      (COMMS conn pin 14) : PA4  (SPI1 NSS pin)
 *   LoRa_Busy  -> SX1268 BUSY     (COMMS conn pin 13) : PC4  (input, NOPULL)
 *   GPIO_INT   -> SX1268 DIO1/IRQ (COMMS conn pin 15) : PB0  (EXTI0, rising)
 *   LoRa_NRST  -> SX1268 NRESET   (COMMS conn pin 5)  : PC5  (output, idle HIGH)
 *   DEPLOY_CMD -> antenna burn    (COMMS conn pin 3)  : PC6  (output, idle LOW)
 *   DEPLOY_SENSE -> antenna switch (COMMS conn pin 4) : PC7  (input, PULLUP;
 *              stowed=LOW, deployed=floating->HIGH; sequence owned by T1.7)
 * Init levels are applied in MX_GPIO_Init() USER CODE (Core/Src/main.c).
 */
#define CS_TTC_GPIO_Port        GPIOA
#define CS_TTC_Pin              GPIO_PIN_4
#define LoRa_Busy_GPIO_Port     GPIOC
#define LoRa_Busy_Pin           GPIO_PIN_4
#define GPIO_INT_GPIO_Port      GPIOB
#define GPIO_INT_Pin            GPIO_PIN_0
#define GPIO_INT_EXTI_IRQn      EXTI0_IRQn
#define LoRa_NRST_GPIO_Port     GPIOC
#define LoRa_NRST_Pin           GPIO_PIN_5
#define DEPLOY_CMD_GPIO_Port    GPIOC
#define DEPLOY_CMD_Pin          GPIO_PIN_6
#define DEPLOY_SENSE_GPIO_Port  GPIOC
#define DEPLOY_SENSE_Pin        GPIO_PIN_7

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
     * Dedicated reset pin (ICD: LoRa_NRST on its own OBC GPIO, idle HIGH).
     * DEPLOY_SENSE is a SEPARATE pin (input + pull-up, owned by T1.7) —
     * never multiplexed with this line.
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
