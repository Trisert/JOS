# Debugging

## Tools

- **IDE:** STM32CubeIDE (Eclipse-based)
- **Debugger:** ST-Link v2/v3
- **GDB:** ARM GNU GDB

## Common Issues

### Hard Fault

**Symptoms:** System stops at `HardFault_Handler`

**Debug steps:**
1. Check stack pointer in debugger
2. Examine LR (Link Register) to find faulting instruction
3. Check memory access violations

### Stack Overflow

**Symptoms:** Random crashes, reboots

**Debug steps:**
1. Enable stack checking in FreeRTOS:
   ```c
   #define configCHECK_FOR_STACK_OVERFLOW 1
   ```
2. Check stack usage: `uxTaskGetStackHighWaterMark()`

### Watchdog Timeout

**Symptoms:** System resets unexpectedly

**Debug steps:**
1. Enable software watchdog logging
2. Check task execution timing
3. Verify `watchdog_alive()` calls

### SPI Communication Errors

**Symptoms:** Communication failures with FRAM, MAX11128, LoRa

**Debug steps:**
1. Check GPIO configuration in CubeMX
2. Verify SPI clock speed
3. Check CS pin polarity (active low/high)

## Debugging Tips

### Printf via ITM

Enable SWV (Serial Wire Viewer) for debug output:

```c
#include "stdio.h"

void dbg_print(const char *msg)
{
    // ITM_SendChar implementation
}
```

### GDB Commands

In GDB session:

```bash
# Break on function
break state_machine_get_state

# Continue
continue

# Print variable
print current_state

# Watch memory location
watch wdg_tasks[0].last_tick

# Backtrace
backtrace
```

### J-Link / ST-Link Debug

```bash
openocd -f interface/stlink.cfg -f target/stm32l4x.cfg
arm-none-eabi-gdb build/JOS.elf
target remote localhost:3333
```

## Logging

For debugging, use the LastStates pool to log state transitions:

```c
laststates_entry_t entry = {
    .timestamp = xTaskGetTickCount(),
    .state_from = old_state,
    .state_to = new_state,
    .trigger = trigger
};
laststates_write(&entry);
```

## Flight vs Debug

- Disable debug prints in flight (performance)
- Use LastStates for post-flight analysis
- Keep hardware watchdog enabled

## Debugging with the ESP32 Simulator

When using the dual-ESP32 verification setup (see [simulation.md](simulation.md)) you can debug both sides:

### Debugging the ESP32-Simulator in CubeIDE

1. Import `simulation/esp32-simulator/` into CubeIDE
2. Connect ESP32-Simulator via JTAG or USB
3. Set breakpoints in `main.c` (console commands, scenario logic)
4. Inspect `sim_state_t` structure to see all sensor values in real time

Useful watchpoints:
```c
sim.bms.soc              // Battery state of charge
sim.bms.voltage_mv       // Battery voltage
sim.imu.accel_x          // X-axis accelerometer
sim.cloud_adc.channels[4] // CLOUD face 0 stripe 4
```

### Debugging the ESP32-OBC in CubeIDE

1. Import `simulation/esp32-obc/` into CubeIDE
2. Connect ESP32-OBC via JTAG or USB
3. Set breakpoints in the JOS source files (state_machine.c, comms.c, etc.)
4. Step through state transitions as the simulator sends data

### Cross-Debugging (STM32 + ESP32)

To debug the STM32 firmware while the ESP32-Simulator feeds it inputs:

1. **Flash the ESP32-Simulator** from command line: `make sim-flash BOARD=sim SIM_PORT=COM4`
2. **Open a serial monitor** for the simulator in a separate window
3. **Connect STM32 via ST-Link** and open a debug session in CubeIDE
4. **Wire UART** from the simulator to the STM32 (see simulation.md wiring table)
5. Set breakpoints in `state_machine.c` → `try_transition()` and `check_battery_autonomous()`
6. Type `bms 20 150 3200` on the simulator console to trigger a CRIT transition
7. Step through the state machine response in the CubeIDE debugger

### Serial Logging

Both ESP32s output logs over USB serial:

- **ESP32-Simulator:** Serial console at 115200 baud (interactive)
- **ESP32-OBC:** ESP-IDF log output at 115200 baud (read-only)

In CubeIDE, open a serial terminal: `Window` → `Show View` → `Terminal`, then connect to the appropriate COM port at 115200 baud.