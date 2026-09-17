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
enum class DioRoute { Idle, Tx, Rx };
static volatile DioRoute g_dio_route = DioRoute::Idle;

/* Mask only the dedicated PB0/EXTI0 radio interrupt. SPI DMA and TIM6 must
 * remain live during RadioLib calls. Never clear pending IRQs on exit: a
 * completion produced by the newly armed operation belongs to that operation. */
class Dio1Guard {
public:
    Dio1Guard() : enabled(NVIC_GetEnableIRQ(EXTI0_IRQn)) {
        NVIC_DisableIRQ(EXTI0_IRQn);
        __DSB();
        __ISB();
    }
    ~Dio1Guard() {
        if (enabled != 0U) { NVIC_EnableIRQ(EXTI0_IRQn); }
    }
    Dio1Guard(const Dio1Guard&) = delete;
    Dio1Guard& operator=(const Dio1Guard&) = delete;
private:
    uint32_t enabled;
};

/* Only after standby + radio IRQ clear succeeded, BEFORE arming new work.
 * SX126x::finishReceive() stops the source before clearing its IRQ status;
 * finishTransmit() alone clears before standby and leaves a race window. */
static int quiesce_dio1(void)
{
    g_dio_route = DioRoute::Idle;
    int16_t result = radio.finishReceive();
    __HAL_GPIO_EXTI_CLEAR_IT(GPIO_INT_Pin);
    NVIC_ClearPendingIRQ(EXTI0_IRQn);
    return (result == RADIOLIB_ERR_NONE) ? 0 : -1;
}

/* Called with DIO1 masked and its old source quiesced. */
static int arm_receive(void)
{
    int16_t result = radio.startReceive();
    if (result == RADIOLIB_ERR_NONE) {
        g_dio_route = DioRoute::Rx;
        return 0;
    }
    (void)quiesce_dio1();
    return -1;
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
    osThreadId_t self = osThreadGetId();
    if (self == NULL || g_tx_wait_handle != NULL) { return -1; }
    Dio1Guard guard;
    if (quiesce_dio1() != 0) { return -1; }
    if ((osThreadFlagsClear(LORA_FLAG_TX_DONE) & 0x80000000U) != 0U) {
        return -1;
    }
    g_tx_wait_handle = self;
    int16_t s = radio.startTransmit(data, (uint8_t)len);
    if (s != RADIOLIB_ERR_NONE) {
        (void)radio.finishTransmit();
        g_tx_wait_handle = NULL;
        if (quiesce_dio1() == 0) { (void)arm_receive(); }
        return -1;
    }
    g_dio_route = DioRoute::Tx;
    return 0;
}

/* The TX owner calls once: success OR timeout closes the transfer. */
extern "C" int lora_tx_wait_done(uint32_t timeout_ms)
{
    if (g_tx_wait_handle == NULL || g_tx_wait_handle != osThreadGetId()) {
        return -1;
    }
    uint32_t flags = osThreadFlagsWait(LORA_FLAG_TX_DONE, osFlagsWaitAny, timeout_ms);
    Dio1Guard guard;
    g_dio_route = DioRoute::Idle;
    int16_t finish = radio.finishTransmit();
    int quiet = quiesce_dio1();
    uint32_t cleared = osThreadFlagsClear(LORA_FLAG_TX_DONE);
    g_tx_wait_handle = NULL;
    int receive = (quiet == 0) ? arm_receive() : -1;
    return ((flags == LORA_FLAG_TX_DONE) && (finish == RADIOLIB_ERR_NONE) &&
            ((cleared & 0x80000000U) == 0U) && (receive == 0)) ? 0 : -1;
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
        return -1;
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
    if (g_tx_wait_handle != NULL) { return -1; }
    Dio1Guard guard;
    if (quiesce_dio1() != 0) { return -1; }
    return arm_receive();
}

/*
 * Called from HAL_GPIO_EXTI_Callback() on the GPIO_INT (DIO1) line.
 * Distinguishes TX_DONE vs RX_DONE by the radio's current mode. The RX/TX tasks
 * register themselves so the right one is woken. (Mirrors RedPill-T ISR design.)
 */
extern "C" void lora_on_dio1_irq(void)
{
    /* No SPI from ISR. Idle suppresses callbacks during failed transitions;
     * the task publishes routing only after the new mode is armed. */
    if (g_dio_route == DioRoute::Tx && g_tx_wait_handle != NULL) {
        osThreadFlagsSet(g_tx_wait_handle, LORA_FLAG_TX_DONE);
    } else if (g_dio_route == DioRoute::Rx && g_rx_handle != NULL) {
        osThreadFlagsSet(g_rx_handle, LORA_FLAG_RX_DONE);
    }
}
