##############################################################################
# Project
##############################################################################
PROJECT_NAME := JOS
TARGET         := build/$(PROJECT_NAME)

##############################################################################
# Toolchain
##############################################################################
PREFIX   := arm-none-eabi-
CC       := $(PREFIX)gcc
CXX      := $(PREFIX)g++
AS       := $(PREFIX)gcc -x assembler-with-cpp
OBJCOPY  := $(PREFIX)objcopy
OBJDUMP  := $(PREFIX)objdump
SIZE     := $(PREFIX)size
NM       := $(PREFIX)nm
RM       := rm -rf
PYTHON   ?= python3

##############################################################################
# MCU flags
##############################################################################
CPU   := -mcpu=cortex-m4
FPU   := -mfpu=fpv4-sp-d16
FLOAT := -mfloat-abi=hard
MCU   := $(CPU) -mthumb $(FPU) $(FLOAT)

##############################################################################
# Directories
##############################################################################
BUILD_DIR   := build
SRC_DIRS    := Core/Src Drivers/STM32L4xx_HAL_Driver/Src Middlewares/Third_Party/FreeRTOS/Source App/obsw App/bms App/memory App/comms App/aocs App/payloads
CXX_SRC_DIRS := Core/Inc/RadioLib/src
ASM_SRC     := Core/Startup/startup_stm32l496vgtx.s
LINKER_SCRIPT := STM32L496VGTX_FLASH.ld

##############################################################################
# Includes
##############################################################################
INCLUDES := \
  -ICore/Inc \
  -ICore/Inc/RadioLib/src \
  -IApp \
  -IApp/obsw \
  -IApp/bms \
  -IApp/memory \
  -IApp/comms \
  -IApp/aocs \
  -IApp/payloads \
  -IDrivers/STM32L4xx_HAL_Driver/Inc \
  -IDrivers/STM32L4xx_HAL_Driver/Inc/Legacy \
  -IMiddlewares/Third_Party/FreeRTOS/Source/include \
  -IMiddlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2 \
  -IMiddlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F \
  -IDrivers/CMSIS/Device/ST/STM32L4xx/Include \
  -IDrivers/CMSIS/Include

##############################################################################
# Defines
##############################################################################
# ---------------------------------------------------------------------------
# Boot-integrity build policy (see App/obsw/boot_crc.h, docs/dev/hardening.md)
#
# Both switches are deliberately assigned with `:=` and NOT with `?=`: with
# `?=` an exported environment variable (a stale shell export, a CI runner
# default, a Docker ENV) silently wins over the makefile and compiles the
# boot-integrity policy out of the flight image without a single warning.
# A `:=` assignment is overridden only by an explicit `make VAR=value` on the
# command line, which is a deliberate act, and even that has to survive the
# 0/1 validation below and prints a banner.
#
#   BOOT_CRC_FATAL=1        reset (bounded) on an untrusted image  [flight]
#   BOOT_CRC_FATAL=0        record + degrade, never reset          [bench]
#   BOOT_CRC_TRUST_UNSTAMPED=0  an unstamped image is NOT trusted  [flight]
#   BOOT_CRC_TRUST_UNSTAMPED=1  unstamped == "nothing to check"    [bench]
# ---------------------------------------------------------------------------
BOOT_CRC_FATAL           := 1
BOOT_CRC_TRUST_UNSTAMPED := 0

# Validation: anything other than 0/1 (typo, empty, "yes", "true") is a hard
# build error instead of a define that quietly evaluates to 0 in #if.
ifneq ($(filter-out 0 1,$(BOOT_CRC_FATAL)),)
  $(error BOOT_CRC_FATAL must be 0 or 1, got '$(BOOT_CRC_FATAL)')
endif
ifneq ($(filter-out 0 1,$(BOOT_CRC_TRUST_UNSTAMPED)),)
  $(error BOOT_CRC_TRUST_UNSTAMPED must be 0 or 1, got '$(BOOT_CRC_TRUST_UNSTAMPED)')
endif
ifeq ($(BOOT_CRC_FATAL),0)
  $(warning *** BOOT_CRC_FATAL=0: the boot-integrity recovery reset is compiled OUT.)
  $(warning *** BENCH BUILD ONLY - this image must never be flashed to flight hardware.)
endif
ifeq ($(BOOT_CRC_TRUST_UNSTAMPED),1)
  $(warning *** BOOT_CRC_TRUST_UNSTAMPED=1: an unstamped image is treated as trusted.)
  $(warning *** BENCH BUILD ONLY - `make crc-stamp` is what a flight image needs.)
endif

BOOT_CRC_DEFS := -DBOOT_CRC_FATAL=$(BOOT_CRC_FATAL) \
                 -DBOOT_CRC_TRUST_UNSTAMPED=$(BOOT_CRC_TRUST_UNSTAMPED)

C_DEFS  := -DDEBUG -DUSE_HAL_DRIVER -DSTM32L496xx
C_DEFS  += $(BOOT_CRC_DEFS)
CXX_DEFS := -DDEBUG -DUSE_HAL_DRIVER -DSTM32L496xx -DRADIOLIB_STATIC_ONLY=1
ASM_DEFS := -DDEBUG

##############################################################################
# C Flags
##############################################################################
CFLAGS  := $(MCU) $(C_DEFS)
CFLAGS += -std=gnu11 -g3 -O0
CFLAGS += -Wall -ffunction-sections -fdata-sections
# -Wundef: a policy switch that is misspelled or never defined must not
# silently evaluate to 0 inside #if. Applied to App/ only (see APP_CFLAGS
# below); the vendored HAL/CMSIS/FreeRTOS trees rely on undefined macros in
# #if by design and would drown the build in warnings.
APP_WFLAGS := -Wundef
CFLAGS += $(INCLUDES)

##############################################################################
# C++ Flags
##############################################################################
CXXFLAGS  := $(MCU) $(CXX_DEFS)
CXXFLAGS += -std=gnu++14 -g3 -O0
CXXFLAGS += -Wall -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti
CXXFLAGS += $(INCLUDES)

##############################################################################
# ASM Flags
##############################################################################
ASFLAGS  := $(MCU) $(ASM_DEFS)
ASFLAGS += -g3
ASFLAGS += $(INCLUDES)

##############################################################################
# Linker Flags
##############################################################################
LDFLAGS := $(MCU)
LDFLAGS += -T$(LINKER_SCRIPT)
LDFLAGS += -specs=nosys.specs -specs=nano.specs
LDFLAGS += -Wl,-Map=$(BUILD_DIR)/$(PROJECT_NAME).map -Wl,--gc-sections
LDFLAGS += -static -lstdc++ -u _printf_float
LDFLAGS += -Wl,--start-group -lc -lm -Wl,--end-group

##############################################################################
# Source & Object files
##############################################################################
C_SOURCES := $(shell find $(SRC_DIRS) -name '*.c' -not -path '*/CMSIS_RTOS_V1/*')
CXX_SOURCES := $(shell find $(CXX_SRC_DIRS) -name '*.cpp' -not -path '*/hal/Arduino/*' -not -path '*/hal/RPi/*' -not -path '*/hal/RPiPico/*' -not -path '*/STM32WLx*')
ASM_SOURCES := $(ASM_SRC)

C_OBJECTS   := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
CXX_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(CXX_SOURCES))
ASM_OBJECTS := $(patsubst %.s,$(BUILD_DIR)/%.o,$(ASM_SOURCES))
OBJECTS     := $(C_OBJECTS) $(CXX_OBJECTS) $(ASM_OBJECTS)

##############################################################################
# Dependencies
##############################################################################
C_DEPS := $(C_OBJECTS:.o=.d)
CXX_DEPS := $(CXX_OBJECTS:.o=.d)
ASM_DEPS := $(ASM_OBJECTS:.o=.d)
DEPS := $(C_DEPS) $(CXX_DEPS) $(ASM_DEPS)

-include $(DEPS)

##############################################################################
# Default target
##############################################################################
.PHONY: all clean flash test coverage crc-stamp sim sim-build sim-flash sim-monitor sim-clean sim-erase

all: $(BUILD_DIR)/$(PROJECT_NAME).elf $(BUILD_DIR)/$(PROJECT_NAME).hex $(BUILD_DIR)/$(PROJECT_NAME).bin

##############################################################################
# Firmware image CRC stamping
##############################################################################
# boot_crc_verify() (App/obsw/boot_crc.c) compares a CRC-32 computed over
# [__fw_image_start, __fw_crc_start) against the word stored in the .fw_crc
# section. A freshly linked image still carries the 0xFFFFFFFF placeholder and
# reports BOOT_CRC_UNSTAMPED; this target patches in the real value.
#
# The expected address of the .fw_crc word is read back out of the ELF so a
# linker-script regression that stops .fw_crc being the last loaded section is
# caught here rather than in flight (fw_crc_stamp.py cross-checks it).
#
# Usage:  make all && make crc-stamp
#
# Refs: ECSS-E-ST-40C 5.4 (software integrity), NASA-STD-8739.8.
CRC_STAMP_TOOL := tools/fw_crc_stamp.py

crc-stamp: $(BUILD_DIR)/$(PROJECT_NAME).bin $(BUILD_DIR)/$(PROJECT_NAME).hex
	@crc_addr=$$($(NM) $(BUILD_DIR)/$(PROJECT_NAME).elf | awk '$$3 == "__fw_crc_start" { print "0x" $$1 }'); \
	if [ -z "$$crc_addr" ]; then \
		echo "error: __fw_crc_start not found in $(BUILD_DIR)/$(PROJECT_NAME).elf" >&2; \
		echo "       is App/obsw/boot_crc.c linked and .fw_crc KEEP()t in $(LINKER_SCRIPT)?" >&2; \
		exit 1; \
	fi; \
	$(PYTHON) $(CRC_STAMP_TOOL) $(BUILD_DIR)/$(PROJECT_NAME).bin \
		--hex $(BUILD_DIR)/$(PROJECT_NAME).hex \
		--crc-addr $$crc_addr --verify

##############################################################################
# Host unit tests (Ceedling + Unity/CMock from the gem)
##############################################################################
# Runs on the build host with the native gcc, not arm-none-eabi-gcc.
# Install once with:  gem install ceedling
test:
	$(MAKE) -C test test

coverage:
	$(MAKE) -C test coverage

##############################################################################
# Verification Simulation (dual ESP32)
##############################################################################
#
# Builds and flashes both ESP32 boards for hardware-in-the-loop testing.
#
# Usage:
#   make sim OBC_PORT=COMx SIM_PORT=COMy       build + flash both
#   make sim-monitor OBC_PORT=COMx SIM_PORT=COMy open two serial monitors
#   make sim-flash SIM_PORT=COMy                 flash only simulator
#   make sim-flash OBC_PORT=COMx                 flash only OBC
#   make sim-clean                                clean both ESP-IDF builds
#   make sim-erase OBC_PORT=COMx                  erase OBC flash
#
# Prerequisites: ESP-IDF v5.x with IDF_PATH set
#
##############################################################################

OBC_PORT  ?= COM3
SIM_PORT  ?= COM4
OBC_PROJ  := simulation/esp32-obc
SIM_PROJ  := simulation/esp32-simulator

sim:
	@echo "=== Building ESP32-OBC ==="
	(cd $(OBC_PROJ) && idf.py -p $(OBC_PORT) flash)
	@echo ""
	@echo "=== Building ESP32-Simulator ==="
	(cd $(SIM_PROJ) && idf.py -p $(SIM_PORT) flash)
	@echo ""
	@echo "=== Both boards flashed. Use 'make sim-monitor' to connect. ==="

sim-build:
	@echo "=== Building ESP32-OBC ==="
	(cd $(OBC_PROJ) && idf.py build)
	@echo ""
	@echo "=== Building ESP32-Simulator ==="
	(cd $(SIM_PROJ) && idf.py build)

sim-flash:
ifndef BOARD
	$(error Usage: make sim-flash OBC_PORT=COMx or make sim-flash SIM_PORT=COMy)
endif
ifeq ($(BOARD),obc)
	(cd $(OBC_PROJ) && idf.py -p $(OBC_PORT) flash)
else ifeq ($(BOARD),sim)
	(cd $(SIM_PROJ) && idf.py -p $(SIM_PORT) flash)
endif

sim-monitor:
	@echo "Opening two serial monitors..."
	@echo "--- ESP32-Simulator (ctrl+] to exit) ---"
	$(start cmd /c "title SIMULATOR & idf.py -p $(SIM_PORT) monitor")
	$(start cmd /c "title OBC & idf.py -p $(OBC_PORT) monitor")

sim-clean:
	$(RM) $(OBC_PROJ)/build
	$(RM) $(SIM_PROJ)/build

sim-erase:
	(cd $(OBC_PROJ) && idf.py -p $(OBC_PORT) erase-flash)

##############################################################################
# Link
##############################################################################
$(BUILD_DIR)/$(PROJECT_NAME).elf: $(OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	@echo "Linking complete"
	$(SIZE) $@

##############################################################################
# Compile C files
##############################################################################
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -MMD -MP -MF"$(@:.o=.d)" $< -o $@

# Flight application code (App/) is held to the stricter warning set: -Wundef
# turns a misspelled or never-defined policy switch (`#if BOOT_CRC_FATL`) into
# a visible warning instead of a silent "0". Scoped to App/ objects only —
# the vendored HAL/CMSIS/FreeRTOS headers pulled in by Core/Src, Drivers/ and
# Middlewares/ use undefined macros in #if by design.
$(filter $(BUILD_DIR)/App/%,$(C_OBJECTS)): CFLAGS += $(APP_WFLAGS)

##############################################################################
# Compile C++ files
##############################################################################
$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c -MMD -MP -MF"$(@:.o=.d)" $< -o $@

##############################################################################
# Assemble
##############################################################################
$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(@D)
	$(AS) $(ASFLAGS) -c -MMD -MP -MF"$(@:.o=.d)" $< -o $@

##############################################################################
# Generate hex and bin
##############################################################################
$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf
	$(OBJCOPY) -O binary -S $< $@

##############################################################################
# Disassembly listing
##############################################################################
$(BUILD_DIR)/$(PROJECT_NAME).list: $(BUILD_DIR)/$(PROJECT_NAME).elf
	$(OBJDUMP) -h -S $< > $@

##############################################################################
# Flash (uses st-link by default, adjust if you use openocd or other)
##############################################################################
# Depends on crc-stamp, never on the raw .bin: flashing an unstamped image
# leaves the CRC word at BOOT_CRC_UNSTAMPED_VALUE, and with the flight policy
# (BOOT_CRC_TRUST_UNSTAMPED=0) the board comes up untrusted and confined to
# STATE_CRIT. `make flash` therefore always programs the stamped binary.
# CubeIDE / OpenOCD "Run" flash the freshly linked ELF and bypass this target
# entirely — see docs/dev/hardening.md 2.4 ("Flashing from the IDE").
flash: crc-stamp
	st-flash write $(BUILD_DIR)/$(PROJECT_NAME).bin 0x08000000

##############################################################################
# Clean
##############################################################################
clean:
	$(RM) $(BUILD_DIR)
