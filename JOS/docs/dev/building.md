# Building the Project

## Prerequisites

- **ARM Toolchain:** `arm-none-eabi-gcc`
- **Make:** GNU Make
- **ST-Link:** For flashing (optional)

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