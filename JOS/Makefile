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
RM       := rm -rf

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
C_DEFS  := -DDEBUG -DUSE_HAL_DRIVER -DSTM32L496xx
CXX_DEFS := -DDEBUG -DUSE_HAL_DRIVER -DSTM32L496xx -DRADIOLIB_STATIC_ONLY=1
ASM_DEFS := -DDEBUG

##############################################################################
# C Flags
##############################################################################
CFLAGS  := $(MCU) $(C_DEFS)
CFLAGS += -std=gnu11 -g3 -O0
CFLAGS += -Wall -ffunction-sections -fdata-sections
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
.PHONY: all clean flash sim sim-build sim-flash sim-monitor sim-clean sim-erase

all: $(BUILD_DIR)/$(PROJECT_NAME).elf $(BUILD_DIR)/$(PROJECT_NAME).hex $(BUILD_DIR)/$(PROJECT_NAME).bin

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
flash: $(BUILD_DIR)/$(PROJECT_NAME).hex
	st-flash write $(BUILD_DIR)/$(PROJECT_NAME).bin 0x08000000

##############################################################################
# Clean
##############################################################################
clean:
	$(RM) $(BUILD_DIR)
