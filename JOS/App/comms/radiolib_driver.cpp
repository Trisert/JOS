/*
 * radiodriver.cpp — C-facing RadioLib driver for the JOS SX1268 (LoRa1268F30).
 *
 * Provides extern "C" entry points consumed by comms.c:
 *   lora_init(), lora_tx(), lora_rx(), lora_tx_wait_done(), lora_on_dio1_irq().
 *
 * Ported/adapted from Marco-42/RedPill-T (satellite/stm32_lora/Core/Src/COMMS.cpp).
 * See radiohal.h for licensing + pin-mapping notes.
 *
 * LoRa params match SPF v3 Table 3.28 + RedPill-T: 436 MHz, BW125, SF10, CR4/8
 * (RadioLib cr=8 is the direct denominator: SX1268.h:36, codingRate reg=cr-4),
 * sync 0x12, 22 dBm, preamble 8, no TCXO, DCDC regulator.
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

/* One radio, two tasks: the beacon task (lora_tx) and the RX task (lora_rx,
   lora_start_receive) both drive the SAME SX1268, the same SPI1 and the same
   DMA completion state in radiolib_hal.cpp. With no serialisation the RX task
   (osPriorityNormal) could preempt the beacon task (BelowNormal) in the middle
   of a RadioLib SPI command sequence. Every entry point that talks to the chip
   holds this mutex for its whole command sequence. Created in lora_init(),
   which main() calls before osKernelInitialize() (legal: osMutexNew() only
   refuses ISR context); NULL before that, when boot is single-threaded. */
static osMutexId_t g_radio_mutex = NULL;

static void radio_lock(void)
{
    if ((g_radio_mutex != NULL) && (osKernelGetState() == osKernelRunning)) {
        (void)osMutexAcquire(g_radio_mutex, osWaitForever);
    }
}

static void radio_unlock(void)
{
    if ((g_radio_mutex != NULL) && (osKernelGetState() == osKernelRunning)) {
        (void)osMutexRelease(g_radio_mutex);
    }
}

/* osThreadFlagsWait() result carries `want` and is not an error code. Same
   rule as comms_flags_have() in comms.c: `==` misses a wanted flag that
   arrived together with another one. */
static bool radio_flags_have(uint32_t flags, uint32_t want)
{
    if ((flags & osFlagsError) != 0U) {
        return false;
    }
    return (flags & want) == want;
}
/* RX task handle, registered by lora_rx_task_create() so the DIO1 ISR can wake
   the correct task on RX_DONE. NULL until the RX task has started. */
static osThreadId_t g_rx_handle = NULL;

/* Called by comms.c once the RX task is created, so the ISR knows whom to wake. */
extern "C" void lora_rx_task_register(osThreadId_t handle)
{
    g_rx_handle = handle;
}

/* Oversize-PHY drops since boot (diagnostics): frames the radio delivered
   that did not fit the caller's buffer and were rejected, never truncated. */
static uint32_t g_rx_oversize_drops = 0U;

extern "C" uint32_t lora_rx_oversize_drops(void)
{
    return g_rx_oversize_drops;
}

extern "C" int lora_init(void)
{
    if (g_radio_mutex == NULL) {
        static const osMutexAttr_t attr = {
            "radio", osMutexPrioInherit, NULL, 0U
        };
        g_radio_mutex = osMutexNew(&attr);
    }

    /* Bind virtual pins to real OBC V2.0 GPIO (radiolib_hal.h, main.h):
       CS_TTC = PA4, RESET = PB1 mux, DIO1 = PB0/EXTI0, BUSY = PC4. */
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
       Signature matches JOS-vendored RadioLib SX1268::begin().
       cr=8 -> coding rate 4/8 per SPF v3 Table 3.28 (RadioLib cr is the direct
       denominator, SX1268.h:36; SX126x_config.cpp: codingRate reg = cr-4). */
    int16_t s = radio.begin(436.0f, 125.0f, 10, 8, 0x12, 22, 8, 0.0f, false);
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
    radio_lock();
    /* Route DIO1 to this task BEFORE the chip can raise TX_DONE. Set after
       startTransmit(), a preemption between the two let TX_DONE reach the RX
       task as a bogus RX_DONE and this task's wait time out. Any DIO1 edge
       that reaches the waiter is therefore only a candidate: lora_tx_wait_done()
       confirms it against the chip's IRQ status (startTransmit() clears that
       status before keying the transmitter), so an RX_DONE still pending from
       the receive mode can no longer pass as TX_DONE. Drop any stale TX_DONE
       bit left over from an earlier timed-out wait. */
    (void)osThreadFlagsClear(LORA_FLAG_TX_DONE);
    g_tx_wait_handle = osThreadGetId();
    int16_t s = radio.startTransmit(data, (uint8_t)len);  /* async; DIO1 -> TX_DONE */
    if (s != RADIOLIB_ERR_NONE) {
        g_tx_wait_handle = NULL;
    }
    radio_unlock();
    return (s == RADIOLIB_ERR_NONE) ? 0 : -1;
}

/* Block the calling task until the SX1268 reports TX_DONE (or timeout).
   A DIO1 wake-up is accepted only when the chip's IRQ status really carries
   TX_DONE; anything else keeps waiting for the rest of the budget. */
extern "C" int lora_tx_wait_done(uint32_t timeout_ms)
{
    const uint32_t start = osKernelGetTickCount();
    uint32_t remaining   = timeout_ms;
    int      rc          = -1;

    for (;;) {
        const uint32_t flags = osThreadFlagsWait(LORA_FLAG_TX_DONE, osFlagsWaitAny,
                                                 remaining);
        if (!radio_flags_have(flags, LORA_FLAG_TX_DONE)) {
            break;                                   /* timeout or error */
        }
        radio_lock();
        const uint32_t irq = radio.getIrqFlags();
        radio_unlock();
        if ((irq & RADIOLIB_SX126X_IRQ_TX_DONE) != 0U) {
            rc = 0;
            break;
        }
        if (timeout_ms != osWaitForever) {
            const uint32_t spent = osKernelGetTickCount() - start;
            if (spent >= timeout_ms) {
                break;
            }
            remaining = timeout_ms - spent;
        }
    }
    g_tx_wait_handle = NULL;
    return rc;
}

extern "C" int lora_rx(uint8_t* buf, size_t* len)
{
    if (buf == NULL || len == NULL) {
        return -1;
    }
    /* In this RadioLib version readData() takes the length by value (no
       writeback), so query the received packet length first and report it
       back to the caller. getPacketLength() must be called BEFORE readData(). */
    radio_lock();
    size_t received = radio.getPacketLength();
    if (received > *len) {
        /* Oversized PHY payload: REJECT, never deliver a truncated frame. A
           silent truncation would hand the validator a well-formed-looking
           prefix of a longer frame (wrong length/CRC semantics) and could
           turn one uplink into a different, dispatchable command.
           *len is left holding the caller's buffer capacity (never overwritten
           with the larger on-air length — the old code reported `received`
           here, claiming the buffer held more bytes than it does), the drop
           is counted, and the caller re-arms. The RX staging buffer is
           COMMS_MAX_PACKET bytes, matching the COMMS_TC_MAX_FRAME validation
           budget. */
        g_rx_oversize_drops++;
        radio_unlock();
        return -1;
    }
    int16_t s = radio.readData(buf, received);
    radio_unlock();
    if (s != RADIOLIB_ERR_NONE) {
        return -1;
    }
    *len = received;
    return 0;
}

extern "C" int lora_start_receive(void)
{
    radio_lock();
    if (g_tx_wait_handle != NULL) {
        /* A transmission is on the air: switching to RX now would abort it.
           The sender re-arms RX itself once its sequence ends
           (lora_send_chunked()). */
        radio_unlock();
        return -1;
    }
    int16_t s = radio.startReceive();
    radio_unlock();
    return (s == RADIOLIB_ERR_NONE) ? 0 : -1;
}

/*
 * Called from HAL_GPIO_EXTI_Callback() on the GPIO_INT (DIO1) line.
 * Distinguishes TX_DONE vs RX_DONE by the radio's current mode. The RX/TX tasks
 * register themselves so the right one is woken. (Mirrors RedPill-T ISR design.)
 */
extern "C" void lora_on_dio1_irq(void)
{
    /* A TX sequence owns DIO1 from before startTransmit() until its wait
       ends; the TX waiter confirms TX_DONE against the IRQ status register
       (lora_tx_wait_done()). Outside a TX sequence DIO1 means RX_DONE. The
       ISR itself never touches SPI. */
    if (g_tx_wait_handle != NULL) {
        osThreadFlagsSet(g_tx_wait_handle, LORA_FLAG_TX_DONE);
    } else if (g_rx_handle != NULL) {
        osThreadFlagsSet(g_rx_handle, LORA_FLAG_RX_DONE);
    }
}
