# ICD OBC ↔ COMMS (software-defined)

Status: **contract + init only**. La sequenza di deploy (firing, retry, timeout) è
fuori scopo — owned by **T1.7**. Questo documento fissa pin, livelli e init.

Antenna-side definitiva (da `TTC_SCH_BoardAntenna.pdf` — **fonte esterna TBD-HW**:
PDF non in repo, pinout connettore da confermare a cura HW/T1.7): il connettore COMMS a
20 pin espone DEPLOY_CMD (3), DEPLOY_SENSE (4), LoRa_NRST (5), SPI_CLK (7),
SPI_MISO (8), SPI_MOSI (9), LoRa_BUSY (13), CS_TTC (14), GPIO_INT/DIO1 (15).
SPI1 = PA5/PA6/PA7 (fisso). Lato OBC non esiste schematico: gli assegnamenti MCU
sotto sono **software-defined** su package LQFP100 (STM32L496VGTx).

## Tabella pin OBC ↔ COMMS ↔ SX1268

| Segnale | MCU (OBC) | COMMS conn | SX1268 | Dir (OBC vista) | Init |
|---|---|---|---|---|---|
| CS_TTC | PA4 | 14 | NSS | OUT PP | HIGH (idle, active-low) |
| LoRa_Busy | PC4 | 13 | BUSY | IN, NOPULL | — |
| GPIO_INT | PB0 / **EXTI0** | 15 | DIO1/IRQ | IN, EXTI rising | NVIC prio 5, enabled |
| LoRa_NRST | PC5 | 5 | NRESET | OUT PP | HIGH (idle, active-low) |
| DEPLOY_CMD | PC6 | 3 | — (burn driver) | OUT PP | LOW (driver off) |
| DEPLOY_SENSE | PC7 | 4 | — (switch) | IN, **PULLUP** | — |
| SPI_CLK / MISO / MOSI | PA5 / PA6 / PA7 | 7 / 8 / 9 | SCK / MISO / MOSI | SPI1 master | CubeMX |

## Scelta pin — motivi

- **CS_TTC = PA4**: pin NSS hardware di SPI1, adiacente a PA5/6/7 (routing corto);
  già `GPIO_Output` in `JOS.ioc`, nessun periferico occupato. De-assertito a HIGH
  per primo in `MX_GPIO_Init()` (il codice generato lo parcheggia a LOW = selezionato).
- **LoRa_Busy = PC4**: GPIO libero (non in `.ioc`, non usato in `App/`), input
  semplice, nessun EXTI consumato.
- **GPIO_INT = PB0 / EXTI0**: linea EXTI0 **libera** (nessun EXTI configurato in
  `JOS.ioc`) con **vettore dedicato** `EXTI0_IRQHandler` (le linee 5–15 condividono
  gli handler `EXTI9_5`/`EXTI15_10`). Evita collisioni EXTI per costruzione:
  nessun altro pin-0 in modo EXTI (PC0 è GPIO_Output in `JOS.ioc`, non EXTI —
  un solo pin per numero può stare su una linea EXTI).
- **LoRa_NRST = PC5**: GPIO libero, output idle HIGH. Pin **dedicato** (il vecchio
  multiplex SPF pag 95 NRST/DEPLOY_SENSE non si usa: la antenna board li espone
  su pin connettore distinti 5 e 4).
- **DEPLOY_CMD = PC6**: GPIO libero, output idle LOW (**polarità burn TBD-HW/T1.7**:
  si assume driver spento a reset, da confermare HW).
- **DEPLOY_SENSE = PC7**: GPIO libero, input con **pull-up interno** (richiesto:
  switch Norm_Open verso GND — **premessa esterna TBD-HW**: da confermare HW/T1.7).
- Tutti: niente SWD (PA13/14), niente SWO (PB3), niente BOOT0/NRST-MCU (pin
  dedicati, non GPIO), niente periferici occupati (SPI2 PB13–15, I2C1 PB6/7,
  I2C2 PB10/11, ADC PC3, TIM1 PE9/11/13, payloads PC0–2 / PE10/12/14, CLOUD CS PB4).

## Livelli DEPLOY_SENSE

Switch antenna Norm_Open verso GND (**premessa esterna TBD-HW**, da confermare
HW/T1.7) + pull-up interno OBC:

| Stato antenna | Switch | PC7 letto |
|---|---|---|
| Stowed | chiuso → GND | **LOW** |
| Deployed | aperto → floating | **HIGH** (pull-up) |

## EXTI / NVIC / FreeRTOS

- `GPIO_MODE_IT_RISING` su PB0, `__HAL_RCC_SYSCFG_CLK_ENABLE()` manuale,
  `HAL_NVIC_SetPriority(EXTI0_IRQn, 5, 0)` + `EnableIRQ`, handler
  `EXTI0_IRQHandler()` → `HAL_GPIO_EXTI_IRQHandler()` → `HAL_GPIO_EXTI_Callback()`
  → `lora_on_dio1_irq()` (in `Core/Src/stm32l4xx_it.c`, sezione USER CODE).
- Priorità **5** = `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (4 bit, gruppo 4 —
  grouping fissato in codice da `HAL_MspInit()`, non generato da CubeMX):
  a livello del syscall ceiling (numericamente ≥ 5), quindi la ISR può chiamare
  API FreeRTOS ISR-safe (`osThreadFlagsSet`).
- **Assunzione DIO1/BUSY sempre pilotati**: SX1268 pilota DIO1 e BUSY in push-pull,
  quindi NOPULL lato OBC è valido (nessun floating in esercizio normale).

## Sorgenti (single source per aspetto)

- Define: `JOS/App/comms/radiolib_hal.h` (C++).
- Init GPIO + NVIC: `MX_GPIO_Init()` sezione `MX_GPIO_Init_2` in `JOS/Core/Src/main.c`
  (solo USER CODE → sopravvive a CubeMX). Literali, non include dell'header C++.
- ISR: `JOS/Core/Src/stm32l4xx_it.c` (solo USER CODE).
- Binding RadioLib: `lora_init()` in `JOS/App/comms/radiolib_driver.cpp` (segue i define).

## Nota per HW: `JOS.ioc` da risincronizzare (solo pin non-EXTI)

`JOS.ioc` **non** è toccato da questo task: handler `EXTI0_IRQHandler()` +
`HAL_GPIO_EXTI_Callback()` (in `Core/Src/stm32l4xx_it.c`) e init PB0/EXTI0/NVIC
(in `MX_GPIO_Init_2`, `Core/Src/main.c`) restano **SCRITTI A MANO in sezioni
USER CODE**. HW **NON** deve abilitare `GPIO_EXTI0` su PB0 né la voce NVIC EXTI0
in CubeMX: la rigenerazione creerebbe init/handler duplicati (conflitto al link
o doppia init). Sync `.ioc` ammessa **solo per gli altri pin**: PA4 =
GPIO_Output (CS_TTC, già così), PC4 = GPIO_Input (Busy), PC5 = GPIO_Output
(NRST), PC6 = GPIO_Output (DEPLOY_CMD), PC7 = GPIO_Input pull-up (DEPLOY_SENSE);
PB0 resta libero in CubeMX. Dopo la sync verificare che le sezioni USER CODE
siano intatte.
