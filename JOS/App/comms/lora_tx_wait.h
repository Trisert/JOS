/*
 * lora_tx_wait.h - TX_DONE confirmation for the SX1268 transmit path.
 *
 * The DIO1 ISR (lora_on_dio1_irq() in radiolib_driver.cpp) can only tell that
 * the radio raised SOME interrupt. During a transmit sequence it wakes the
 * sender with LORA_FLAG_TX_DONE, and the sender confirms the wake-up against
 * the chip's IRQ status before it treats the transmission as complete. This
 * logic is plain C so it can be host-tested; the only radio access it needs
 * is lora_irq_status(), which the flight driver implements.
 */
#ifndef LORA_TX_WAIT_H
#define LORA_TX_WAIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thread flags the DIO1 ISR sets on the task it routes the interrupt to. */
#define LORA_FLAG_TX_DONE  0x01U
#define LORA_FLAG_RX_DONE  0x02U

/* SX126x GetIrqStatus bit 0, TxDone (RadioLib
   src/modules/SX126x/SX126x_registers.h:159, RADIOLIB_SX126X_IRQ_TX_DONE).
   radiolib_driver.cpp pins the two values equal with a static_assert. */
#define LORA_IRQ_TX_DONE   0x0001U

/* Current SX126x IRQ status word, read under the radio mutex. Implemented by
   radiolib_driver.cpp in the flight build. A failed SPI read returns 0, so an
   unreadable status is never taken as TX_DONE (fail closed). */
uint32_t lora_irq_status(void);

/* Block the calling task until TX_DONE is confirmed, or timeout_ms ticks
   (1 tick = 1 ms, configTICK_RATE_HZ = 1000) have elapsed.

   Waits for LORA_FLAG_TX_DONE; each wake-up is accepted only when
   lora_irq_status() carries LORA_IRQ_TX_DONE. Any other wake-up (an RX_DONE
   still pending from receive mode, for instance) keeps waiting for the rest
   of the budget. osWaitForever waits without a bound.

   Returns 0 on a confirmed TX_DONE, -1 on timeout or on an RTOS error. */
int lora_tx_wait_confirm(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* LORA_TX_WAIT_H */
