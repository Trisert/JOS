/**
 ******************************************************************************
 * @file    usart1_dbg.h
 * @brief   USART1 debug/telemetry UART (OBC V2.0: PB6 TX / PB7 RX, J6 17/18).
 *
 * OBC V2.0 netlist puts USART1_TX/USART_RX on PB6/PB7 (the umbilical
 * debug/telemetry port, 115200-8-N-1). The STM32Cube HAL UART/USART drivers
 * are NOT vendored in Drivers/ (only 21 HAL modules are — same reason
 * Core/Src/hw_watchdog.c drives the IWDG at register level), so this module
 * initialises USART1 directly against the CMSIS register definitions
 * (RM0351 38/40-style USART programming: BRR, CR1 UE/TE/RE) and offers a
 * minimal polled TX path. No interrupts, no DMA, no RX buffering: this is a
 * bring-up/debug port, not a comms link.
 *
 * Declared in JOS.ioc as USART1/Asynchronous/115200 alongside the pin mux so
 * a CubeMX regeneration keeps the pins owned (PB6/PB7 must NOT be reused for
 * I2C1 — I2C1_FRAM lives on PB8/PB9 per the netlist).
 ******************************************************************************
 */

#ifndef USART1_DBG_H
#define USART1_DBG_H

#ifdef __cplusplus
extern "C" {
#endif

/** Baud rate of the debug port (8 data bits, no parity, 1 stop bit). */
#define USART1_DBG_BAUD  115200U

/**
 * @brief  Enable clocks, mux PB6/PB7 to AF7, set BRR from the live PCLK2
 *         frequency and enable TX + RX (8N1, oversampling by 16).
 * @note   Idempotent-safe to call once from main() before the scheduler
 *         starts. Uses HAL only for RCC/GPIO (already vendored); the USART
 *         itself is register-level.
 */
void usart1_dbg_init(void);

/**
 * @brief  Blocking single-character transmit (polled TXE).
 * @param  c character to send.
 * @retval c.
 */
int usart1_dbg_putc(int c);

/**
 * @brief  Blocking NUL-terminated string transmit.
 * @param  s string to send (must be NUL-terminated).
 */
void usart1_dbg_write(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* USART1_DBG_H */
