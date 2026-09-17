/* Compile the production driver with strict hardware/RTOS doubles, not its
 * replacement C stub. No hardware I/O occurs in this test. */
#define RADIOLIB_HAL_H
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "cmsis_os.h"
#define RLIB_NSS 0
#define RLIB_RESET 1
#define RLIB_DIO1 2
#define RLIB_BUSY 3
#define CS_TTC_GPIO_Port 0
#define CS_TTC_Pin 4
#define LoRa_NRST_GPIO_Port 0
#define LoRa_NRST_Pin 1
#define GPIO_INT_GPIO_Port 0
#define GPIO_INT_Pin 0
#define LoRa_Busy_GPIO_Port 0
#define LoRa_Busy_Pin 4
#define GPIO_PIN_SET 1
#define RADIOLIB_ERR_NONE 0
extern "C" void lora_on_dio1_irq(void);
#define EXTI0_IRQn 6
static bool irq_enabled=true, exti_pending=false, nvic_pending=false;
static bool late_finish=false, immediate_tx=false, immediate_rx=false;
static unsigned rx_notifications=0;
static void deliver_irq() {
 if(irq_enabled && (nvic_pending || exti_pending)) {
  nvic_pending=false; exti_pending=false; lora_on_dio1_irq();
 }
}
static void dio_edge() { exti_pending=true; nvic_pending=true; deliver_irq(); }
static uint32_t NVIC_GetEnableIRQ(int n) { assert(n==EXTI0_IRQn); return irq_enabled; }
static void NVIC_DisableIRQ(int n) { assert(n==EXTI0_IRQn); irq_enabled=false; }
static void NVIC_EnableIRQ(int n) { assert(n==EXTI0_IRQn); irq_enabled=true; deliver_irq(); }
static void NVIC_ClearPendingIRQ(int n) { assert(n==EXTI0_IRQn); nvic_pending=false; }
static void __HAL_GPIO_EXTI_CLEAR_IT(int n) { assert(n==GPIO_INT_Pin); exti_pending=false; }
static void __DSB() {}
static void __ISB() {}
static int hspi1;
static void HAL_GPIO_WritePin(int,int,int) {}
class STM32Hal {
public:
 explicit STM32Hal(int*) {}
 void addPin(int,int,int) {}
 void configureResetPin() {}
 void pulseReset() {}
};
class Module { public: Module(STM32Hal*,int,int,int,int) {} };
class SX1268 {
public:
 enum Mode { Sleep, Rx, Tx, Standby } mode=Sleep;
 int tx_error=0, finish_error=0, rx_error=0, quiet_error=0;
 unsigned finishes=0, receives=0;
 explicit SX1268(Module*) {}
 int begin(float,float,int,int,int,int,int,float,bool) { return 0; }
 int sleep() { mode=Sleep; return 0; }
 int startTransmit(const uint8_t*,uint8_t) {
  mode=Tx; if(immediate_tx) dio_edge(); return tx_error;
 }
 int finishTransmit() {
  ++finishes; if(late_finish) dio_edge(); mode=Standby; return finish_error;
 }
 int finishReceive() { mode=Standby; return quiet_error; }
 int startReceive() { ++receives; mode=Rx; if(immediate_rx) dio_edge(); return rx_error; }
 size_t getPacketLength() { return 1; }
 int readData(uint8_t*,size_t) { return 0; }
};
/* Stateful thread-flag double: flags persist until consumed by a matching
 * wait, exactly like CMSIS-RTOS2. This is what makes a DIO1 that races the
 * wait timeout observable: the flag it set survives into the next transmit. */
static osThreadId_t self=(void*)1;
static osThreadId_t rx_sentinel=(void*)2;
static uint32_t thread_flags=0;
static int inject_dio1_on_timeout=0;
static int set_calls=0;
static osThreadId_t set_targets[16];
static uint32_t set_flag_values[16];
extern "C" void lora_on_dio1_irq(void);
osThreadId_t osThreadGetId() { return self; }
osStatus_t osThreadFlagsSet(osThreadId_t t,uint32_t f) {
 if(set_calls<16) { set_targets[set_calls]=t; set_flag_values[set_calls]=f; }
 ++set_calls;
 if(t==self) thread_flags|=f;
 if(t==rx_sentinel) ++rx_notifications;
 return osOK;
}
uint32_t osThreadFlagsClear(uint32_t f) {
 uint32_t before=thread_flags;
 thread_flags&=~f;
 return before;
}
uint32_t osThreadFlagsWait(uint32_t want,uint32_t,uint32_t) {
 if(thread_flags&want) {
  uint32_t got=thread_flags&want;
  thread_flags&=(uint32_t)~want;
  return got;
 }
 if(inject_dio1_on_timeout) lora_on_dio1_irq(); /* DIO1 racing the expiry */
 return 0xFFFFFFFEu; /* matches no single flag bit: timeout */
}
#include "../../App/comms/radiolib_driver.cpp"
int main() {
 uint8_t byte=0;
 assert(lora_init()==0);
 lora_rx_task_register(rx_sentinel);
 assert(lora_start_receive()==0);

 /* TX success: DIO1 completes it; teardown restores RX. */
 assert(lora_tx(&byte,1)==0);
 lora_on_dio1_irq();
 assert(lora_tx_wait_done(2000)==0);
 assert(radio.finishes==1 && radio.mode==SX1268::Rx);

 /* TX timeout: no DIO1; cleanup still runs and RX is re-armed. */
 assert(lora_tx(&byte,1)==0);
 assert(lora_tx_wait_done(2000)==-1);
 assert(radio.finishes==2 && radio.mode==SX1268::Rx);

 /* startTransmit failure: finishTransmit cleanup + RX re-arm before the
  * caller sees the error (asserts both, per CodeRabbit review). */
 radio.tx_error=-1;
 assert(lora_tx(&byte,1)==-1);
 assert(radio.finishes==3 && radio.mode==SX1268::Rx);
 radio.tx_error=0;

 /* finishTransmit failure is reported, RX still re-armed. */
 radio.finish_error=-1;
 assert(lora_tx(&byte,1)==0);
 lora_on_dio1_irq();
 assert(lora_tx_wait_done(2000)==-1);
 assert(radio.mode==SX1268::Rx);
 radio.finish_error=0;

 /* CodeRabbit R7: a DIO1 that races the wait timeout leaves TX_DONE set
  * (CMSIS keeps unconsumed thread flags). The next transmission must
  * reject the stale flag instead of reporting instant success, and while
  * it owns the radio a DIO1 must reach the TX owner, never the RX task. */
 inject_dio1_on_timeout=1;
 assert(lora_tx(&byte,1)==0);
 assert(lora_tx_wait_done(2000)==-1);   /* timeout; DIO1 fires inside the window */
 inject_dio1_on_timeout=0;
 unsigned f0=radio.finishes;
 assert(lora_tx(&byte,1)==0);           /* new transmission owns the radio */
 for(int i=0;i<set_calls && i<16;i++) assert(set_targets[i]!=rx_sentinel);
 assert(lora_tx_wait_done(2000)==-1);   /* stale flag rejected; timeout closes TX */
 assert(radio.mode==SX1268::Rx);
 assert(lora_tx(&byte,1)==0);           /* a new transfer, not a second wait */
 lora_on_dio1_irq();
 assert(lora_tx_wait_done(2000)==0);
 assert(radio.finishes==f0+2 && radio.mode==SX1268::Rx);
 /* A late edge inside finishTransmit is pending in BOTH EXTI and NVIC.
  * It must be drained before RX routing, never delivered to RX or next TX. */
 late_finish=true;
 assert(lora_tx(&byte,1)==0);
 assert(lora_tx_wait_done(2000)==-1);
 assert(irq_enabled && !exti_pending && !nvic_pending);
 assert(thread_flags==0 && rx_notifications==0);
 late_finish=false;

 /* Old pending RX edge at next start cannot impersonate a TX completion. */
 irq_enabled=false; dio_edge(); thread_flags=1;
 assert(lora_tx(&byte,1)==0);
 assert(!irq_enabled && !exti_pending && !nvic_pending && thread_flags==0);
 NVIC_EnableIRQ(EXTI0_IRQn);
 assert(lora_tx_wait_done(2000)==-1);

 /* A new completion before startTransmit returns must NOT be discarded. */
 immediate_tx=true;
 assert(lora_tx(&byte,1)==0);
 assert(thread_flags==1);
 assert(lora_tx_wait_done(2000)==0);
 immediate_tx=false;

 /* Same for the first received packet while RX is being armed. */
 immediate_rx=true;
 assert(lora_start_receive()==0);
 assert(rx_notifications==1);
 immediate_rx=false;

 /* RX rearm error and quiesce failure must leave routing idle. */
 radio.rx_error=-1;
 assert(lora_tx(&byte,1)==0);
 dio_edge();
 assert(lora_tx_wait_done(2000)==-1);
 dio_edge(); assert(rx_notifications==1 && thread_flags==0);
 radio.rx_error=0;
 radio.quiet_error=-1;
 assert(lora_tx(&byte,1)==-1);
 dio_edge(); assert(rx_notifications==1 && thread_flags==0);
 radio.quiet_error=0;

 /* Restore disabled state on success and on failed start. */
 irq_enabled=false;
 assert(lora_tx(&byte,1)==0);
 assert(lora_tx_wait_done(2000)==-1);
 assert(!irq_enabled);
 radio.tx_error=-1;
 assert(lora_tx(&byte,1)==-1 && !irq_enabled);
 radio.tx_error=0;
 NVIC_EnableIRQ(EXTI0_IRQn);
 assert(lora_tx_wait_done(2000)==-1); /* no active owner */
 puts("radio lifecycle: cleanup, failures, stale/late IRQ, immediate TX/RX, IRQ restoration PASS");
}
