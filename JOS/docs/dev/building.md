# Building the Project

## Prerequisites

- **ARM Toolchain:** `arm-none-eabi-gcc`
- **Make:** GNU Make
- **ST-Link:** For flashing (optional)
- **ESP-IDF v5.x:** For verification simulation (optional, see [simulation.md](simulation.md))

## Installing Toolchain

### Windows (via ARM toolchain installer)
1. Download ARM GNU toolchain from [ARM Developer Website](https://developer.arm.com/tools-and-software/open-source-software/developer-tools/gnu-toolchain/gnu-rm)
2. Install and add to PATH: `C:\Program Files (x86)\Arm\gnu-arm-eabi\bin`

### Linux (Debian/Ubuntu)
```bash
sudo apt-get install gcc-arm-none-eabi
```

### macOS
```bash
brew install --cask gcc-arm-embedded
```

## Building

From the project root:

```bash
make
```

This produces:
- `build/JOS.elf` - ELF binary
- `build/JOS.hex` - Intel HEX format
- `build/JOS.bin` - Raw binary for flashing
- `build/JOS.map` - Memory map
- `build/JOS.list` - Disassembly listing

## Cleaning

```bash
make clean
```

## Flashing

### Using ST-Link (STMicroelectronics)
```bash
make flash
```

This runs:
```bash
st-flash write build/JOS.bin 0x08000000
```

### Using OpenOCD
```bash
openocd -f interface/stlink.cfg -f target/stm32l4x.cfg -c "program build/JOS.elf verify reset exit"
```

## Build Options

### Debug Build (default)
```bash
make CFLAGS="-DDEBUG"  # adds -g3 -O0
```

### Release Build
```bash
make CFLAGS="-DRELEASE"  # adds -Os
```

## Troubleshooting

### "arm-none-eabi-gcc: command not found"
Ensure the ARM toolchain is in your PATH.

### "No rule to make target"
Ensure you're in the project root directory (where Makefile is located).

### Build errors about missing headers
Run from the project root. The Makefile uses relative paths.

### Flash errors
- Check ST-Link driver installation
- Verify MCU is in bootloader mode (BOOT0 pin)
- Try: `st-info --probe` to verify connection

## Verification Simulation

For hardware-in-the-loop testing without real satellite hardware, use the dual ESP32 simulation:

```bash
make sim OBC_PORT=COM3 SIM_PORT=COM4
```

### Building in STM32CubeIDE

The STM32 firmware and the ESP32 simulator projects can both be managed from CubeIDE:

1. **STM32 project:** `File` → `Open Projects from File System` → select the JOS root directory. The `.project` and `.cproject` files will be detected automatically.

2. **ESP32 simulator:** `File` → `Open Projects from File System` → select `simulation/esp32-simulator/`. Requires ESP-IDF tools installed (see [simulation.md](simulation.md) for setup).

3. **Build all:** Use the hammer icon on each project, or register `make sim` as an external tool (`Run` → `External Tools Configurations`).

See [simulation.md](simulation.md) for full documentation including CubeIDE-specific instructions.