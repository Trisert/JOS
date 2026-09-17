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
 int tx_error=0, finish_error=0, rx_error=0;
 unsigned finishes=0, receives=0;
 explicit SX1268(Module*) {}
 int begin(float,float,int,int,int,int,int,float,bool) { return 0; }
 int sleep() { mode=Sleep; return 0; }
 int startTransmit(const uint8_t*,uint8_t) { mode=Tx; return tx_error; }
 int finishTransmit() { ++finishes; mode=Standby; return finish_error; }
 int startReceive() { ++receives; mode=Rx; return rx_error; }
 size_t getPacketLength() { return 1; }
 int readData(uint8_t*,size_t) { return 0; }
};
static uint32_t wait_result=1;
osThreadId_t osThreadGetId() { return reinterpret_cast<void*>(1); }
uint32_t osThreadFlagsWait(uint32_t,uint32_t,uint32_t) { return wait_result; }
osStatus_t osThreadFlagsSet(osThreadId_t,uint32_t) { return osOK; }
#include "../../App/comms/radiolib_driver.cpp"
int main() {
 uint8_t byte=0;
 assert(lora_init()==0);
 assert(lora_start_receive()==0);
 assert(lora_tx(&byte,1)==0);
 assert(lora_tx_wait_done(2000)==0);
 assert(radio.finishes==1 && radio.mode==SX1268::Rx);
 wait_result=0xFFFFFFFEu;
 assert(lora_tx(&byte,1)==0);
 assert(lora_tx_wait_done(2000)==-1);
 assert(radio.finishes==2 && radio.mode==SX1268::Rx);
 radio.tx_error=-1;
 assert(lora_tx(&byte,1)==-1);
 assert(radio.mode==SX1268::Rx);
 radio.tx_error=0; wait_result=1; radio.rx_error=-1;
 assert(lora_tx(&byte,1)==0);
 assert(lora_tx_wait_done(2000)==-1);
 radio.rx_error=0; radio.finish_error=-1;
 assert(lora_tx(&byte,1)==0);
 assert(lora_tx_wait_done(2000)==-1);
 assert(radio.mode==SX1268::Rx);
 puts("radio lifecycle: success, timeout, start failure, rearm failure PASS");
}
