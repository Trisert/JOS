/**
 * @file    test_lora_tx_wait.c
 * @brief   Unit tests for the SX1268 TX_DONE confirmation loop
 *          (App/comms/lora_tx_wait.c).
 *
 * The DIO1 ISR only knows that the radio raised SOME interrupt. During a
 * transmit sequence it wakes the sender, and lora_tx_wait_confirm() must
 * accept that wake-up only when the chip's IRQ status carries TX_DONE; any
 * other wake-up (an RX_DONE still pending from receive mode) keeps waiting
 * for the rest of the budget. The RTOS is a CMock mock of fakes/cmsis_os.h;
 * lora_irq_status() - the flight driver's SPI read - is scripted here.
 */

#include "unity.h"
#include "lora_tx_wait.h"
#include "mock_cmsis_os.h"

#include <stdint.h>

#define FLAGS_ERROR_TIMEOUT  0xFFFFFFFEU   /* CMSIS-RTOS2 osFlagsErrorTimeout */
#define IRQ_RX_DONE          0x0002U       /* SX126x GetIrqStatus bit 1        */

/* Scripted IRQ status words, one per lora_irq_status() call. */
static uint32_t irq_script[8];
static int      irq_script_len;
static int      irq_calls;

uint32_t lora_irq_status(void)
{
    TEST_ASSERT_TRUE_MESSAGE(irq_calls < irq_script_len,
                             "unexpected lora_irq_status() call");
    return irq_script[irq_calls++];
}

static void script_irq(const uint32_t *words, int n)
{
    for (int i = 0; i < n; i++) {
        irq_script[i] = words[i];
    }
    irq_script_len = n;
    irq_calls      = 0;
}

void setUp(void)
{
    irq_script_len = 0;
    irq_calls      = 0;
}

void tearDown(void)
{
}

/* Nominal path: one wake-up, the chip confirms TX_DONE. */
void test_tx_done_confirmed_on_first_wake(void)
{
    const uint32_t irq[] = { LORA_IRQ_TX_DONE };
    script_irq(irq, 1);

    osKernelGetTickCount_ExpectAndReturn(100u);
    osThreadFlagsWait_ExpectAndReturn(LORA_FLAG_TX_DONE, osFlagsWaitAny, 2000u,
                                      LORA_FLAG_TX_DONE);

    TEST_ASSERT_EQUAL_INT(0, lora_tx_wait_confirm(2000u));
    TEST_ASSERT_EQUAL_INT(1, irq_calls);
}

/* A wake-up whose IRQ status carries RX_DONE, not TX_DONE, is not a completed
   transmission: the loop waits again with the REMAINING budget and accepts the
   real TX_DONE that follows. Before the fix any DIO1 wake-up counted. */
void test_rx_done_wake_is_not_taken_for_tx_done(void)
{
    const uint32_t irq[] = { IRQ_RX_DONE, LORA_IRQ_TX_DONE };
    script_irq(irq, 2);

    osKernelGetTickCount_ExpectAndReturn(100u);
    osThreadFlagsWait_ExpectAndReturn(LORA_FLAG_TX_DONE, osFlagsWaitAny, 2000u,
                                      LORA_FLAG_TX_DONE);
    osKernelGetTickCount_ExpectAndReturn(350u);          /* 250 ms spent */
    osThreadFlagsWait_ExpectAndReturn(LORA_FLAG_TX_DONE, osFlagsWaitAny, 1750u,
                                      LORA_FLAG_TX_DONE);

    TEST_ASSERT_EQUAL_INT(0, lora_tx_wait_confirm(2000u));
    TEST_ASSERT_EQUAL_INT(2, irq_calls);
}

/* An unreadable status (the driver returns 0 on a failed SPI read) is never
   TX_DONE; when the budget is spent the wait fails instead of looping. */
void test_unconfirmed_wakes_exhaust_the_budget(void)
{
    const uint32_t irq[] = { 0u };
    script_irq(irq, 1);

    osKernelGetTickCount_ExpectAndReturn(0u);
    osThreadFlagsWait_ExpectAndReturn(LORA_FLAG_TX_DONE, osFlagsWaitAny, 2000u,
                                      LORA_FLAG_TX_DONE);
    osKernelGetTickCount_ExpectAndReturn(2000u);         /* budget gone */

    TEST_ASSERT_EQUAL_INT(-1, lora_tx_wait_confirm(2000u));
}

/* No wake-up at all: the RTOS timeout is reported, and the chip is not read. */
void test_timeout_is_a_failure_without_reading_the_chip(void)
{
    osKernelGetTickCount_ExpectAndReturn(0u);
    osThreadFlagsWait_ExpectAndReturn(LORA_FLAG_TX_DONE, osFlagsWaitAny, 2000u,
                                      FLAGS_ERROR_TIMEOUT);

    TEST_ASSERT_EQUAL_INT(-1, lora_tx_wait_confirm(2000u));
    TEST_ASSERT_EQUAL_INT(0, irq_calls);
}

/* A result without the TX_DONE flag (another flag only) is not a wake-up for
   this wait. */
void test_other_thread_flag_is_a_failure(void)
{
    osKernelGetTickCount_ExpectAndReturn(0u);
    osThreadFlagsWait_ExpectAndReturn(LORA_FLAG_TX_DONE, osFlagsWaitAny, 2000u,
                                      LORA_FLAG_RX_DONE);

    TEST_ASSERT_EQUAL_INT(-1, lora_tx_wait_confirm(2000u));
    TEST_ASSERT_EQUAL_INT(0, irq_calls);
}

/* osWaitForever is passed through unchanged on every wait, and the elapsed
   time is not consulted. */
void test_wait_forever_keeps_waiting_without_a_bound(void)
{
    const uint32_t irq[] = { IRQ_RX_DONE, LORA_IRQ_TX_DONE | IRQ_RX_DONE };
    script_irq(irq, 2);

    osKernelGetTickCount_ExpectAndReturn(0u);
    osThreadFlagsWait_ExpectAndReturn(LORA_FLAG_TX_DONE, osFlagsWaitAny,
                                      osWaitForever, LORA_FLAG_TX_DONE);
    osThreadFlagsWait_ExpectAndReturn(LORA_FLAG_TX_DONE, osFlagsWaitAny,
                                      osWaitForever, LORA_FLAG_TX_DONE);

    TEST_ASSERT_EQUAL_INT(0, lora_tx_wait_confirm(osWaitForever));
}
