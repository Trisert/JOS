/*
 * lora_tx_wait.c - TX_DONE confirmation for the SX1268 transmit path.
 * See lora_tx_wait.h.
 */
#include "lora_tx_wait.h"

#include "cmsis_os.h"

#ifndef osFlagsError
#define osFlagsError 0x80000000U   /* CMSIS-RTOS2 cmsis_os2.h */
#endif

int lora_tx_wait_confirm(uint32_t timeout_ms)
{
    const uint32_t start     = osKernelGetTickCount();
    uint32_t       remaining = timeout_ms;

    for (;;) {
        const uint32_t flags = osThreadFlagsWait(LORA_FLAG_TX_DONE, osFlagsWaitAny,
                                                 remaining);
        if (((flags & osFlagsError) != 0U) ||
            ((flags & LORA_FLAG_TX_DONE) == 0U)) {
            return -1;                               /* timeout or error */
        }
        if ((lora_irq_status() & LORA_IRQ_TX_DONE) != 0U) {
            return 0;
        }
        /* DIO1 fired, but not for TX_DONE: keep waiting for what is left. */
        if (timeout_ms != osWaitForever) {
            const uint32_t spent = osKernelGetTickCount() - start;
            if (spent >= timeout_ms) {
                return -1;
            }
            remaining = timeout_ms - spent;
        }
    }
}
