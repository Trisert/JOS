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
# Warning / hardening flags (shared by C and C++)
##############################################################################
#
# -Wall -Wextra          : broad diagnostic coverage (NASA Power of Ten rule 10,
#                          JPL-182, ECSS-E-ST-40C: compile at max warning level).
# -Werror=implicit-function-declaration
#                        : REQUIRED-ERROR. GCC >= 14 rejects implicit
#                          declarations outright while GCC 10 (the Ubuntu CI
#                          toolchain) only warns, so CI could stay green on code
#                          that does not build with a modern toolchain. This
#                          caught a real defect (see PR #5), hence it is
#                          promoted explicitly on every toolchain.
# -Wdouble-promotion     : flags accidental float -> double promotion. The
#                          Cortex-M4F FPU is single precision only; every
#                          promotion drops into slow soft-float libgcc code.
#
# -Werror is intentionally NOT enabled globally: the vendored third-party trees
# (STM32 HAL, FreeRTOS, RadioLib) emit warnings we neither own nor want to
# patch, and turning those into hard errors would block the build for reasons
# unrelated to flight-software quality.
#
WARNFLAGS   := -Wall -Wextra -Wdouble-promotion

# C-only: C++ has no implicit function declarations, and cc1plus rejects the
# option with "'-Werror=' argument ... is not valid for C++" on every C++ TU.
C_WARNFLAGS := $(WARNFLAGS) -Werror=implicit-function-declaration

##############################################################################
# C Flags
##############################################################################
CFLAGS  := $(MCU) $(C_DEFS)
CFLAGS += -std=gnu11 -g3 -O0
CFLAGS += $(C_WARNFLAGS) -ffunction-sections -fdata-sections
CFLAGS += $(INCLUDES)

##############################################################################
# C++ Flags
##############################################################################
CXXFLAGS  := $(MCU) $(CXX_DEFS)
CXXFLAGS += -std=gnu++14 -g3 -O0
CXXFLAGS += $(WARNFLAGS) -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti
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
# Static analysis (cppcheck) -- SINGLE SOURCE OF TRUTH
##############################################################################
#
# `.github/workflows/build.yml` runs `make -C JOS cppcheck` (and
# `make -C JOS cppcheck-canary`). Nothing about the analysis is duplicated in
# the workflow, so the gate cannot drift from the real build: the include paths
# come from $(INCLUDES) and the defines from $(C_DEFS) above.
#
# cppcheck is invoked from the *repository root* (one directory above this
# Makefile) so that every reported path and every path-scoped suppression is
# repo-relative (`JOS/...`), matching what CI logs show.
#
# Scope: first-party code only -- `JOS/App` and the hand-written CubeMX
# `JOS/Core/Src` sources. The vendored trees (STM32 HAL, CMSIS, FreeRTOS,
# RadioLib) are reachable via -I so headers resolve, but they are NEVER
# analysed; their findings are silenced with PATH-SCOPED suppressions so that
# no check id is ever disabled for first-party code.
#
CPPCHECK ?= cppcheck

# PINNED. A cppcheck bump can add, drop or retune checks and would silently
# change the gate result on a runner-image refresh, so CI installs exactly this
# version and the `cppcheck` target refuses to run anything else. To bump:
# change this value, re-run `make cppcheck`, then fix or document every new
# finding in the same PR.
CPPCHECK_VERSION ?= 2.13.0

# This Makefile lives in <repo>/JOS, so the repo root is one level up.
CPPCHECK_ROOT   := ..
CPPCHECK_PREFIX := $(notdir $(CURDIR))/

# First-party analysis roots (relative to this Makefile).
CPPCHECK_SRC := App Core/Src

# Vendored / CubeMX-generated translation units that happen to sit inside
# Core/Src: the ST asm330lhh register driver and the newlib syscall stubs.
CPPCHECK_EXCLUDE := \
  Core/Src/asm330lhh_reg.c \
  Core/Src/syscalls.c \
  Core/Src/sysmem.c \
  Core/Src/system_stm32l4xx.c

CPPCHECK_SUPPRESS := \
  --suppress=missingIncludeSystem \
  --suppress=unusedFunction \
  --suppress=checkersReport \
  --suppress=unmatchedSuppression \
  --suppress=toomanyconfigs \
  --suppress=normalCheckLevelMaxBranches \
  '--suppress=*:$(CPPCHECK_PREFIX)Drivers/*' \
  '--suppress=*:$(CPPCHECK_PREFIX)Middlewares/*' \
  '--suppress=*:$(CPPCHECK_PREFIX)Core/Inc/RadioLib/*' \
  '--suppress=constParameterPointer:$(CPPCHECK_PREFIX)Core/Src/stm32l4xx_hal_msp.c'

# Notes on the suppressions above (deliberately narrow -- no real check id is
# switched off repo-wide):
#   missingIncludeSystem / unusedFunction / checkersReport /
#   unmatchedSuppression / toomanyconfigs / normalCheckLevelMaxBranches
#     -- cppcheck bookkeeping messages, not defects.
#   *:JOS/Drivers/* , *:JOS/Middlewares/* , *:JOS/Core/Inc/RadioLib/*
#     -- PATH-SCOPED. Vendored code we do not own (HAL, CMSIS, FreeRTOS,
#        RadioLib). Only used as include paths; never analysed.
#   constParameterPointer:JOS/Core/Src/stm32l4xx_hal_msp.c
#     -- FILE-SCOPED, one id. The HAL MSP callbacks are weak overrides whose
#        signatures are fixed by <stm32l4xx_hal_*.h> (e.g.
#        `void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi)`); adding const would
#        conflict with the HAL prototype and the file is CubeMX-generated, so
#        an inline suppression outside a USER CODE block would be regenerated
#        away. Every other check still runs on this file.
#
# NOT suppressed anywhere: variableScope, constParameter, constParameterPointer
# (outside the one generated file above), arrayIndexOutOfBounds, nullPointer,
# uninitvar, ... -- i.e. every id that can indicate a real defect.

# -D__GNUC__=10 : Drivers/CMSIS/Include/cmsis_compiler.h ends in an
#                 `#error Unknown compiler` when no compiler macro is defined,
#                 which aborts the whole configuration and makes cppcheck exit
#                 0 without analysing anything. 10 == the CI toolchain
#                 (gcc-arm-none-eabi 10.3).
# --platform=arm32-wchar_t4 : the target is 32-bit ARM ILP32 with 4-byte
#                 wchar_t; the default native (x86-64 LP64) model gives wrong
#                 sizeof()/overflow reasoning for firmware.
CPPCHECK_FLAGS := \
  --enable=all \
  --std=c11 \
  --error-exitcode=1 \
  --inline-suppr \
  --platform=arm32-wchar_t4 \
  -D__GNUC__=10 \
  $(C_DEFS) \
  $(patsubst -I%,-I$(CPPCHECK_PREFIX)%,$(INCLUDES)) \
  $(CPPCHECK_SUPPRESS) \
  $(addprefix -i,$(addprefix $(CPPCHECK_PREFIX),$(CPPCHECK_EXCLUDE)))

CPPCHECK_TARGETS := $(addprefix $(CPPCHECK_PREFIX),$(CPPCHECK_SRC))
CPPCHECK_CANARY_DIR := $(CURDIR)/$(BUILD_DIR)/cppcheck-canary

.PHONY: cppcheck cppcheck-version cppcheck-canary cppcheck-print

# Fail loudly if the installed cppcheck is not the pinned one.
cppcheck-version:
	@found=$$($(CPPCHECK) --version 2>/dev/null | awk '{print $$2}'); \
	if [ -z "$$found" ]; then \
	  echo "ERROR: '$(CPPCHECK)' not found. Install cppcheck $(CPPCHECK_VERSION)."; exit 1; \
	fi; \
	if [ "$$found" != "$(CPPCHECK_VERSION)" ]; then \
	  echo "ERROR: cppcheck $(CPPCHECK_VERSION) is pinned but $$found is installed."; \
	  echo "       Findings are version dependent -- install the pinned version,"; \
	  echo "       or bump CPPCHECK_VERSION in JOS/Makefile deliberately."; \
	  exit 1; \
	fi; \
	echo "cppcheck $$found (pinned: $(CPPCHECK_VERSION))"

# Echo the exact command CI runs (handy for debugging / docs).
cppcheck-print:
	@echo "cd $(CPPCHECK_ROOT) && $(CPPCHECK) $(CPPCHECK_FLAGS) $(CPPCHECK_TARGETS)"

cppcheck: cppcheck-version
	@echo "=== cppcheck: first-party scope ($(CPPCHECK_SRC)) ==="
	cd $(CPPCHECK_ROOT) && $(CPPCHECK) $(CPPCHECK_FLAGS) $(CPPCHECK_TARGETS)

# Canary: prove the gate is still able to fail.
#
# Generates a throwaway C file with two unmistakable defects (out-of-bounds
# array write + NULL dereference), runs cppcheck over it with the EXACT same
# $(CPPCHECK_FLAGS) the real gate uses, and fails if cppcheck comes back clean.
#
# The canary deliberately #includes "main.h" and "FreeRTOS.h" so it walks the
# same HAL/CMSIS/FreeRTOS-portable include chain as real first-party code. That
# is what makes it catch the regression class this gate was broken by: if
# `-D__GNUC__` or the ARM_CM4F portable include path go missing, cmsis_compiler.h
# fires `#error Unknown compiler` and cppcheck abandons the configuration
# WITHOUT analysing anything -- and if someone then silences that with
# `--suppress=preprocessorErrorDirective`, cppcheck exits 0 and the gate is
# vacuous. In that state the canary exits 0 too, and this target fails the build.
cppcheck-canary: cppcheck-version
	@rm -rf $(CPPCHECK_CANARY_DIR)
	@mkdir -p $(CPPCHECK_CANARY_DIR)
	@printf '%s\n' \
	  '/* GENERATED BY `make cppcheck-canary` - deliberately buggy, deleted below. */' \
	  '#include <stddef.h>' \
	  '#include "main.h"      /* pulls in HAL + CMSIS (needs -D__GNUC__)      */' \
	  '#include "FreeRTOS.h"  /* pulls in portmacro.h (needs the ARM_CM4F -I) */' \
	  'int cppcheck_canary_oob(void);' \
	  'int cppcheck_canary_null(void);' \
	  'int cppcheck_canary_oob(void)' \
	  '{' \
	  '    int buf[4] = {0, 0, 0, 0};' \
	  '    buf[7] = 1;            /* arrayIndexOutOfBounds */' \
	  '    return buf[0];' \
	  '}' \
	  'int cppcheck_canary_null(void)' \
	  '{' \
	  '    int *p = NULL;' \
	  '    *p = 42;               /* nullPointer */' \
	  '    return *p;' \
	  '}' \
	  > $(CPPCHECK_CANARY_DIR)/canary.c
	@echo "=== cppcheck canary: the gate must REJECT this file ==="
	@set +e; \
	 ( cd $(CPPCHECK_ROOT) && $(CPPCHECK) $(CPPCHECK_FLAGS) $(CPPCHECK_CANARY_DIR)/canary.c ); \
	 rc=$$?; \
	 rm -rf $(CPPCHECK_CANARY_DIR); \
	 if [ $$rc -eq 0 ]; then \
	   echo "CANARY FAILED: cppcheck exited 0 on a file with an out-of-bounds"; \
	   echo "write and a NULL dereference. The static-analysis gate is not"; \
	   echo "actually gating -- check the flags/suppressions in JOS/Makefile."; \
	   exit 1; \
	 fi; \
	 echo "canary OK: cppcheck exited $$rc on the buggy file (gate is live)"

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
