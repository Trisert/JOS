/*
 * radiodriver.cpp — C-facing RadioLib driver for the JOS SX1268 (LoRa1268F30).
 *
 * Provides extern "C" entry points consumed by comms.c:
 *   lora_init(), lora_tx(), lora_rx(), lora_tx_wait_done(), lora_on_dio1_irq().
 *
 * Ported/adapted from Marco-42/RedPill-T (satellite/stm32_lora/Core/Src/COMMS.cpp).
 * See radiohal.h for licensing + pin-mapping notes.
 *
 * LoRa params match SPF pag 90 + RedPill-T: 436 MHz, BW125, SF10, CR4/5
 * (codingRate=5), sync 0x12, 22 dBm, preamble 8, no TCXO, DCDC regulator.
 *
 * TX is ASYNC (startTransmit completes on DIO1 TX_DONE). lora_tx() fires and
 * returns; callers that send multiple chunks MUST lora_tx_wait_done() between
 * chunks or the next chunk will overwrite the buffer mid-air (B2 gap, plan).
 */

#include "radiolib_hal.h"
#include "cmsis_os.h"   /* osThreadFlagsX for TX_DONE signalling */

/* RadioLib objects.
 * JOS-vendored RadioLib: SX1268 takes a Module* (not (hal,cs,dio1,rst,busy)).
 * Build the Module from the virtual pin IDs first. */
STM32Hal radioHal(&hspi1);
Module   radioModule(&radioHal, RLIB_NSS, RLIB_DIO1, RLIB_RESET, RLIB_BUSY);
SX1268   radio(&radioModule);

/* Thread flag used to wake the TX path on DIO1 TX_DONE. */
#define LORA_FLAG_TX_DONE 0x01U
#define LORA_FLAG_RX_DONE 0x02U

static osThreadId_t g_tx_wait_handle = NULL;

extern "C" int lora_init(void)
{
    /* Bind virtual pins to real CubeMX GPIO (placeholders until OBC schematic). */
    radioHal.addPin(RLIB_NSS,   CS_TTC_GPIO_Port,     CS_TTC_Pin);
    radioHal.addPin(RLIB_RESET, LoRa_NRST_GPIO_Port,  LoRa_NRST_Pin);
    radioHal.addPin(RLIB_DIO1,  GPIO_INT_GPIO_Port,   GPIO_INT_Pin);
    radioHal.addPin(RLIB_BUSY,  LoRa_Busy_GPIO_Port,  LoRa_Busy_Pin);

    /* CS idle HIGH (inactive). */
    HAL_GPIO_WritePin(CS_TTC_GPIO_Port, CS_TTC_Pin, GPIO_PIN_SET);

    /* Reset/deploy multiplexed pin -> configure as output, idle HIGH. */
    radioHal.configureResetPin();
    radioHal.pulseReset();

    /* begin(freq, bw, sf, cr, syncWord, power, preamble, tcxo, useLDO).
       Signature matches JOS-vendored RadioLib SX1268::begin(). */
    int16_t s = radio.begin(436.0f, 125.0f, 10, 5, 0x12, 22, 8, 0.0f, false);
    if (s != RADIOLIB_ERR_NONE) {
        return -1;
    }

    /* Put radio to sleep; RX is armed by the RX task via lora_start_receive(). */
    radio.sleep();
    return 0;
}

extern "C" int lora_tx(const uint8_t* data, size_t len)
{
    if (data == NULL || len == 0U) {
        return -1;
    }
    /* RadioLib startTransmit takes a uint8_t length; reject oversized payloads
       instead of silently truncating (would corrupt the frame). */
    if (len > 255U) {
        return -1;
    }
    g_tx_wait_handle = osThreadGetId();
    int16_t s = radio.startTransmit(data, (uint8_t)len);  /* async; DIO1 -> TX_DONE */
    if (s != RADIOLIB_ERR_NONE) {
        g_tx_wait_handle = NULL;
        return -1;
    }
    return 0;
}

/* Block the calling task until DIO1 signals TX_DONE (or timeout). */
extern "C" int lora_tx_wait_done(uint32_t timeout_ms)
{
    uint32_t flags = osThreadFlagsWait(LORA_FLAG_TX_DONE, osFlagsWaitAny, timeout_ms);
    g_tx_wait_handle = NULL;
    return (flags == LORA_FLAG_TX_DONE) ? 0 : -1;
}

extern "C" int lora_rx(uint8_t* buf, size_t* len)
{
    if (buf == NULL || len == NULL) {
        return -1;
    }
    /* In this RadioLib version readData() takes the length by value (no
       writeback), so query the received packet length first and report it
       back to the caller. getPacketLength() must be called BEFORE readData(). */
    size_t received = radio.getPacketLength();
    if (received > *len) {
        received = *len;   /* truncate to buffer capacity */
    }
    int16_t s = radio.readData(buf, received);
    if (s != RADIOLIB_ERR_NONE) {
        return -1;
    }
    *len = received;
    return 0;
}

extern "C" int lora_start_receive(void)
{
    int16_t s = radio.startReceive();
    return (s == RADIOLIB_ERR_NONE) ? 0 : -1;
}

/*
 * Called from HAL_GPIO_EXTI_Callback() on the GPIO_INT (DIO1) line.
 * Distinguishes TX_DONE vs RX_DONE by the radio's current mode. The RX/TX tasks
 * register themselves so the right one is woken. (Mirrors RedPill-T ISR design.)
 */
extern "C" void lora_on_dio1_irq(void)
{
    /* Heuristic: if a TX is pending, it's TX_DONE; else assume RX_DONE.
       A tighter check would read the SX1268 IRQ status register. */
    if (g_tx_wait_handle != NULL) {
        osThreadFlagsSet(g_tx_wait_handle, LORA_FLAG_TX_DONE);
    } else {
        /* RX task should have registered its handle; signal via a shared flag.
           For scaffolding, the RX task polls lora_start_receive() + reads on
           its own flag — wire the RX handle similarly to g_tx_wait_handle. */
        osThreadFlagsSet(NULL, LORA_FLAG_RX_DONE);  /* placeholder: replace with RX handle */
    }
}
