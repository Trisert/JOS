# Verification Simulation

Hardware-in-the-loop (HIL) verification setup using two ESP32 boards to test the JOS On-Board Software without real satellite hardware.

## Overview

```
 PC (serial console)
      |
      +---> ESP32-Simulator
      |       |  SPI slave      | UART (commands/sensor data)
      |       +-----------------+---------> ESP32-OBC
      |                                        Runs JOS OBSW
      |                                        FreeRTOS tasks
      |                                        State machine
      +---[monitor both via serial]-----------+
```

The **ESP32-OBC** runs a ported version of the JOS OBSW. The **ESP32-Simulator** replaces all external hardware (sensors, battery, ground station) with controllable simulated inputs.

## Quick Start

```bash
# 1. Set ESP-IDF environment
# (Windows) %IDF_PATH%\export.bat
# (Linux/macOS) source $IDF_PATH/export.sh

# 2. Build and flash both boards
make sim OBC_PORT=COM3 SIM_PORT=COM4

# 3. Open serial monitors in separate terminals
make sim-monitor OBC_PORT=COM3 SIM_PORT=COM4
```

On the simulator console, type `help` to see available commands.

## Wiring

| Signal | OBC ESP32 (GPIO) | Simulator ESP32 (GPIO) |
|--------|-------------------|------------------------|
| SPI MOSI | GPIO23 | GPIO23 |
| SPI MISO | GPIO19 | GPIO19 |
| SPI SCLK | GPIO18 | GPIO18 |
| SPI CS   | GPIO5  | GPIO5  |
| UART TX  | GPIO17 | GPIO16 |
| UART RX  | GPIO16 | GPIO17 |

Connect both ESP32s ground pins together. No external pull-ups needed for UART.

## Make Targets

| Target | Description |
|--------|-------------|
| `make sim OBC_PORT=.. SIM_PORT=..` | Build and flash both ESP32s |
| `make sim-build` | Build both without flashing |
| `make sim-flash BOARD=obc OBC_PORT=..` | Flash only the OBC board |
| `make sim-flash BOARD=sim SIM_PORT=..` | Flash only the simulator board |
| `make sim-monitor OBC_PORT=.. SIM_PORT=..` | Open serial monitors |
| `make sim-clean` | Delete both build directories |
| `make sim-erase OBC_PORT=..` | Erase OBC flash |

## Simulator Console Commands

### Status and Control
| Command | Description |
|---------|-------------|
| `help` | Show all commands |
| `start` | Start simulation |
| `stop` | Stop simulation |
| `status` | Print all sensor values |
| `reset` | Reset to default values |
| `speed <mult>` | Set time multiplier (1-1000x) |
| `auto` | Toggle automatic scenario mode |

### Battery (BMS/EPS)
| Command | Description |
|---------|-------------|
| `bms <soc> <temp_x10> <voltage_mv>` | Set battery state |
| `drain <rate>` | Drain battery at rate (%/min) |
| `charge <rate>` | Charge battery at rate (%/min) |

Examples:
```
bms 85 250 4100       SoC=85%, 25.0C, 4100mV
drain 5               Battery drains 5%/min
bms 20 150 3200       Low battery scenario
```

### Sensors
| Command | Description |
|---------|-------------|
| `imu <ax> <ay> <az> <gx> <gy> <gz>` | Set IMU (raw 16-bit) |
| `mag <x> <y> <z>` | Set magnetometer (raw 16-bit) |
| `adc <ch4> <ch5> <ch8> <ch9>` | Set ADC channels (mV) |

Examples:
```
imu 0 0 1000 50 30 200    Nominal pointing
mag 200 0 40000           Earth magnetic field
adc 1500 1200 800 750     Payload readings
```

### Telecommands (Ground Station)
| Command | Description |
|---------|-------------|
| `cmd <id> [hex]` | Send raw telecommand to OBC |
| `activate` | Send ACTIVATE_PAYLOAD (0x05) |
| `ready` | Send EXIT_STATE (0x02) |
| `reset_obc` | Send RESET (0x01) |

Examples:
```
cmd 6 000003E8          Set beacon interval to 1000ms
cmd 5                   Activate payloads
activate                Same as cmd 5
```

### Scenarios
| Command | Description |
|---------|-------------|
| `scenario <name>` | Run a preset scenario |
| `list` | List available scenarios |

## Preset Scenarios

### `nominal`
Normal satellite operations. Automatically:
1. Waits for boot (OBC: OFF → INIT → READY)
2. Sends ACTIVATE_PAYLOAD after 5s
3. Monitors payload operations

```bash
scenario nominal
```

### `low_battery`
Battery drain to critical, then recovery:
1. SoC starts at 85%, drains at 5%/min
2. OBC should transition READY → CRIT (SoC < 40%)
3. Battery charges back at 3%/min
4. OBC should recover CRIT → READY (SoC >= 80%)

```bash
scenario low_battery
```

### `debris_impact`
Simulates micro-debris impact on CLOUD faces:
1. Waits 3s, then modifies CLOUD ADC stripe values
2. Simulates partial recovery after 10s

```bash
scenario debris_impact
```

### `full_orbit`
Complete 90-minute orbit simulation (compressed to ~9 min at 10x):
1. Sunlight phase: battery charging, ADC active
2. Eclipse phase: battery draining, ADC zeroed
3. Payload operations phase
4. Reports final SoC

```bash
speed 10
scenario full_orbit
```

## Communication Protocol

The two ESP32s communicate over a binary protocol defined in `simulation/shared/sim_protocol.h`.

### Packet Format
```
[4 bytes header][up to 56 bytes payload]
```

| Field | Offset | Size | Description |
|-------|--------|------|-------------|
| magic | 0 | 4 | 0x53494D48 ("SIMH") |
| version | 4 | 1 | Protocol version |
| cmd | 5 | 1 | Command ID |
| len | 6 | 1 | Payload length |
| seq | 7 | 1 | Sequence number |

### Key Commands
| CMD | Direction | Description |
|-----|-----------|-------------|
| 0x80 (DATA) | Sim → OBC | BMS sensor data |
| 0x81 (DATA+1) | Sim → OBC | ADC channel data |
| 0x41 (LORA_RX) | Sim → OBC | Simulated uplink telecommand |
| 0x51 (NOTIFY_GPIO) | OBC → Sim | GPIO state change notification |

## Project Structure

```
simulation/
├── shared/
│   └── sim_protocol.h          # Binary protocol definitions
├── esp32-obc/                  # JOS OBSW ported to ESP-IDF
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   ├── main/
│   │   ├── main.c              # app_main, peripheral init, task creation
│   │   ├── main.h              # STM32 peripheral handle stubs
│   │   ├── cmsis_os.h          # CMSIS-RTOS v2 → FreeRTOS shim
│   │   ├── stm32l4xx_hal.h     # HAL header redirect
│   │   ├── sim_bridge.h        # Bridge API to simulator
│   │   └── obsw_stubs.h        # Stub declarations for init functions
│   └── components/
│       └── hal_compat/
│           ├── hal_compat.h    # STM32 HAL type/function stubs
│           ├── hal_compat.c    # Implementations (GPIO, SPI, I2C, ADC)
│           └── CMakeLists.txt
└── esp32-simulator/            # Satellite environment simulator
    ├── CMakeLists.txt
    ├── sdkconfig.defaults
    ├── main/
    │   ├── main.c              # Console, simulation engine, SPI slave
    │   ├── sim_state.h         # Global simulation state
    │   ├── sim_sensors.h/c     # Sensor data generators
    │   └── sim_scenarios.h/c   # Preset scenario engine
    └── components/
        ├── sim_sensors/
        └── sim_comms/
```

## ESP32-OBC HAL Compatibility

The OBSW code uses STM32 HAL and CMSIS-RTOS APIs. These are mapped to ESP-IDF equivalents:

| STM32 API | ESP-IDF Implementation |
|-----------|----------------------|
| `HAL_GPIO_WritePin` | Internal GPIO state tracking |
| `HAL_SPI_Transmit/Receive` | Stubbed (data flows via simulator bridge) |
| `HAL_I2C_Master_Transmit` | Stubbed (sensor data from UART) |
| `HAL_ADC_GetValue` | Returns simulator-controlled values |
| `osThreadNew` | `xTaskCreate` |
| `osMutexNew/Acquire/Release` | `xSemaphoreCreateMutex/Take/Give` |
| `osDelay` | `vTaskDelay` |
| `NVIC_SystemReset` | `esp_restart()` |

## Troubleshooting

### "idf.py: command not found"
ESP-IDF environment not activated. Run the export script for your platform.

### Simulator not responding
1. Check UART wiring (TX↔RX crossover)
2. Verify both ESP32s share a common ground
3. Check baud rates match (115200)

### OBC doesn't receive sensor data
1. Verify `start` was typed on the simulator console
2. Check SPI connections (MOSI↔MOSI, MISO↔MISO, SCLK↔SCLK)
3. Ensure CS pin is connected

### State machine stuck in CRIT
The battery SoC is below the recovery threshold (80%). Use `bms 100 250 4200` then wait for the 10 Hz check loop to trigger recovery.

### Build fails with missing headers
Run `make sim-clean` then rebuild. Ensure the `simulation/shared/` directory exists.
