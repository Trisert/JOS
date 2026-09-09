/**
 ******************************************************************************
 * @file    usart1_dbg.c
 * @brief   USART1 debug/telemetry UART, register-level (see usart1_dbg.h).
 ******************************************************************************
 */

#include "usart1_dbg.h"

#include "main.h"   /* stm32l4xx_hal.h: RCC/GPIO helpers, USART1, CMSIS bits */

/* PB6/PB7 alternate function for USART1_TX/RX (datasheet DS12212 AF map). */
#define USART1_DBG_GPIO_AF  GPIO_AF7_USART1

void usart1_dbg_init(void)
{
    GPIO_InitTypeDef gpio = { 0 };
    uint32_t pclk;
    uint32_t brr;

    /* Clocks: USART1 on APB2, pins on GPIOB (CubeMX: RCC.USART1Freq_Value). */
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /**USART1 GPIO Configuration
     * PB6  ------> USART1_TX
     * PB7  ------> USART1_RX
     */
    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = USART1_DBG_GPIO_AF;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* BRR for oversampling-by-16: BRR = round(PCLK2 / baud) holds
     * mantissa[11:0]:fraction[3:0], i.e. USARTDIV*16. At the nominal 80 MHz
     * PCLK2 / 115200 Bd this yields 694 (43 + 6/16, 0.06 % fast) — well
     * within the ~2 % async tolerance. Computed from the live clock so a
     * future clock-tree change cannot silently corrupt the baud rate. */
    pclk = HAL_RCC_GetPCLK2Freq();
    brr = (pclk + (USART1_DBG_BAUD / 2U)) / USART1_DBG_BAUD;
    /* Keep the 12:4 field width: clamp rather than wrap on absurd clocks. */
    if (brr > 0xFFFFU) {
        brr = 0xFFFFU;
    } else if (brr < 16U) {
        brr = 16U;
    }
    USART1->BRR = brr;

    /* 8N1, oversampling by 16 (OVER8 = 0), no flow control: CR1/CR2/CR3 reset
     * values already encode this, so only enable TX, RX and the peripheral. */
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE;
    USART1->CR1 |= USART_CR1_UE;
}

int usart1_dbg_putc(int c)
{
    /* Polled TX: TXE = transmit data register empty, ready for TDR write. */
    while ((USART1->ISR & USART_ISR_TXE) == 0U) {
    }
    USART1->TDR = (uint32_t)(uint8_t)c;
    return c;
}

void usart1_dbg_write(const char *s)
{
    if (s == NULL) {
        return;
    }
    while (*s != '\0') {
        usart1_dbg_putc((int)*s);
        s++;
    }
}
