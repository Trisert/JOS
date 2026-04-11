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