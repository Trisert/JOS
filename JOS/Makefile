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
SRC_DIRS    := Core/Src Drivers/STM32L4xx_HAL_Driver/Src Middlewares/Third_Party/FreeRTOS/Source
CXX_SRC_DIRS := Core/Inc/RadioLib/src
ASM_SRC     := Core/Startup/startup_stm32l496vgtx.s
LINKER_SCRIPT := STM32L496VGTX_FLASH.ld

##############################################################################
# Includes
##############################################################################
INCLUDES := \
  -ICore/Inc \
  -ICore/Inc/RadioLib/src \
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
.PHONY: all clean flash

all: $(BUILD_DIR)/$(PROJECT_NAME).elf $(BUILD_DIR)/$(PROJECT_NAME).hex $(BUILD_DIR)/$(PROJECT_NAME).bin

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
