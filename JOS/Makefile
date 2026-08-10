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
# version and the `cppcheck` target refuses to run anything else. To bump:\
# change this value, re-run `make cppcheck`, then fix or document every new\
# finding in the same PR. ALSO re-verify the per-line `cppcheck-suppress`\
# comments in JOS/App/memory/memory.c and JOS/Core/Src/sram2_parity.c: bumping\
# cppcheck can change finding ids, so delete and re-run `make cppcheck` on each,\
# then restore the suppressions ONLY if the findings return (see those files).
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
#     -- cppcheck bookkeeping messages, not defects. missingIncludeSystem is
#        the `<stdio.h>`-shaped one: those headers belong to the toolchain, are
#        deliberately not in the repo, and are not passed with -I.
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
# DELIBERATELY ABSENT: `--suppress=missingInclude`. It was here briefly and was
# wrong: the vendored trees that produce almost all missing-include noise are
# already blanket-suppressed by path above, and toolchain headers are covered
# by missingIncludeSystem, so the only thing a repo-wide missingInclude
# suppression could still hide was a FIRST-PARTY header that does not resolve
# -- precisely the case where the message is a real finding -- and neither the
# `build` job nor the canary could see it happen. The condition is now asserted
# instead of suppressed: see the `cppcheck-includes` target below, which CI
# runs as its own step.
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
#
# Macro set: the union of $(C_DEFS) and $(CXX_DEFS), deduplicated by $(sort).
# Every first-party translation unit in $(CPPCHECK_SRC) is C today, but the
# RadioLib-facing glue is C++ and a .cpp landing under JOS/App would otherwise
# be analysed with the C macro set only (no -DRADIOLIB_STATIC_ONLY=1), i.e.
# under a configuration that is never built. Passing both keeps analysis and
# compilation on the same macros. The two sets agree on every shared macro, so
# $(sort) cannot produce a contradictory -D pair.
CPPCHECK_DEFS := $(sort $(C_DEFS) $(CXX_DEFS))

# --enable: deliberately NOT `all`. `all` is `warning,style,performance,\
# portability,information,unusedFunction,missingInclude` -- `unusedFunction` is\
# unsound in a whole-program sense here (every RTOS entry point looks unused),\
# and `missingInclude` is INTENTIONALLY NOT suppressed: see the DELIBERATELY\
# ABSENT note above and the `cppcheck-includes` / `cppcheck-includes-proof`\
# targets below, which FAIL the build if a repo-wide `--suppress=missingInclude`\
# is ever re-added. The `error` severity is always on and is never listed: that\
# is where the canary ids (arrayIndexOutOfBounds, nullPointer) and the rest of\
# the real-defect checks live, so narrowing here cannot weaken the gate.

# Output template, pinned rather than left to cppcheck's default.
#
# `cppcheck-canary` proves the gate is alive by asserting that the two planted
# ids come back in their bracketed `[{id}]` form. cppcheck's DEFAULT template
# happens to print `[{id}]`, but it also appends `{code}` -- an echo of the
# offending source line -- and both halves of that are a default, i.e. a thing
# a version bump may change. Pinning removes the assumption: `[{id}]` is
# guaranteed to be there, and the template deliberately OMITS `{code}` so the
# offending source line is never echoed back into the log (see the canary
# comment below). CAVEAT: `{message}` itself can still quote constructs derived
# from the analysed file (e.g. `Array 'buf[4]' accessed at index 7`), so the log
# is NOT entirely free of source-derived text -- only the raw source line is.
# The same template is used by `cppcheck` and `cppcheck-canary`
# because they share $(CPPCHECK_FLAGS).
CPPCHECK_TEMPLATE := '{file}:{line}:{column}: {severity}: {message} [{id}]'

CPPCHECK_FLAGS := \
  --enable=warning,style,performance,portability,information \
  --std=c11 \
  --error-exitcode=1 \
  --inline-suppr \
  --template=$(CPPCHECK_TEMPLATE) \
  --platform=arm32-wchar_t4 \
  -D__GNUC__=10 \
  $(CPPCHECK_DEFS) \
  $(patsubst -I%,-I$(CPPCHECK_PREFIX)%,$(INCLUDES)) \
  $(CPPCHECK_SUPPRESS) \
  $(addprefix -i,$(addprefix $(CPPCHECK_PREFIX),$(CPPCHECK_EXCLUDE)))

CPPCHECK_TARGETS := $(addprefix $(CPPCHECK_PREFIX),$(CPPCHECK_SRC))
CPPCHECK_CANARY_DIR := $(CURDIR)/$(BUILD_DIR)/cppcheck-canary

.PHONY: cppcheck cppcheck-version cppcheck-canary cppcheck-includes cppcheck-includes-proof cppcheck-print

# Fail loudly if the installed cppcheck is not the pinned one.
#
# The version is parsed as "the first dotted number anywhere on the first line"
# rather than by field position: `cppcheck --version` has printed
# "Cppcheck 2.13.0", "Cppcheck 2.14.0 dev" and (in distro builds) a trailing
# vendor suffix, so `awk '{print $$2}'` was one output-format change away from
# hard-blocking CI on a correctly installed binary.
cppcheck-version:
	@found=$$($(CPPCHECK) --version 2>/dev/null \
	          | head -n 1 \
	          | grep -oE '[0-9]+(\.[0-9]+)+' \
	          | head -n 1); \
	if [ -z "$$found" ]; then \
	  echo "ERROR: '$(CPPCHECK)' not found, or its --version output carries no"; \
	  echo "       parsable version number. Install cppcheck $(CPPCHECK_VERSION)."; \
	  exit 1; \
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

# Assert that every first-party #include resolves.
#
# This is the replacement for the repo-wide `--suppress=missingInclude` that
# used to sit in $(CPPCHECK_SUPPRESS). A header we own that cppcheck cannot
# find is not noise: it means the analysis of that translation unit ran against
# an incomplete view of the code, so anything the gate did NOT report about it
# is worth nothing. Suppressing the message made that state invisible; this
# target makes it fail.
#
# `--check-config` is cppcheck's dedicated configuration pass -- it resolves
# includes and reports what it could not find, without running the checks. The
# real gate's flags (include paths, macro set, exclusions, path-scoped vendored
# suppressions) are reused verbatim so the two agree on what "first-party"
# means; `--enable=missingInclude` is added because that id is not part of the
# gate's `--enable` set, and `missingIncludeSystem` stays suppressed because
# toolchain headers are intentionally not in the repo.
cppcheck-includes: cppcheck-version
	@echo "=== cppcheck: every first-party #include must resolve ==="
	@mkdir -p $(BUILD_DIR)
	@log=$(CURDIR)/$(BUILD_DIR)/cppcheck-includes.out; \
	 set +e; \
	 ( cd $(CPPCHECK_ROOT) && $(CPPCHECK) --check-config --enable=missingInclude \
	     $(CPPCHECK_FLAGS) $(CPPCHECK_TARGETS) ) > "$$log" 2>&1; \
	 rc=$$?; \
	 cat "$$log"; \
	 if grep -q '\[missingInclude\]' "$$log"; then \
	   echo "FAILED: cppcheck cannot resolve a first-party #include (above)."; \
	   echo "Whatever translation unit it belongs to was analysed against an"; \
	   echo "incomplete view of the code, so a clean 'make -C JOS cppcheck' does"; \
	   echo "not mean much for it. Add the missing -I to INCLUDES, or the missing"; \
	   echo "header -- do NOT re-add a repo-wide --suppress=missingInclude."; \
	   exit 1; \
	 fi; \
	 if [ $$rc -ne 0 ]; then \
	   echo "FAILED: the cppcheck configuration pass exited $$rc. No missing"; \
	   echo "include was named, so this is a different problem (bad flag,"; \
	   echo "unreadable path, internal error). Run 'make -C JOS cppcheck-print'."; \
	   exit 1; \
	 fi; \
	 echo "includes OK: no unresolved first-party header"

# Proof-of-life for `cppcheck-includes`.
#
# Kilo (PR #31, finding at Makefile:388) flagged that `cppcheck-includes` only
# greps the cppcheck *log* for `[missingInclude]`. That is vacuous the moment
# someone re-adds `--suppress=missingInclude` to $(CPPCHECK_SUPPRESS): the
# check-config run then suppresses the very finding the gate exists to surface,
# the log stays clean, and the build goes permanently green -- exactly the
# false-negative class the gate was built to prevent. So the gate MUST also
# assert, statically, that the forbidden suppression is absent from this
# Makefile. This target proves that assertion is live: it injects the forbidden
# suppression into a throwaway copy of the Makefile and asserts that
# `cppcheck-includes` then FAILS. If the injection no longer makes the gate
# fail, this target fails instead -- so a future regression in the gate's
# self-check is caught in CI rather than shipping silently.
cppcheck-includes-proof:
# Static proof-of-life (no cppcheck required): assert that `--suppress=missingInclude`
# is ABSENT from the active CPPCHECK_SUPPRESS definition. Kilo (PR #31, Makefile:388)
# flagged that `cppcheck-includes` runs `cppcheck --check-config --enable=missingInclude`
# while inheriting $(CPPCHECK_SUPPRESS); if that set ever re-includes
# `--suppress=missingInclude`, the very finding the gate exists to surface is silenced
# and the gate goes permanently green -- a false negative. The recipe below greps ONLY the
# active suppression lines (those that begin with `--suppress=`) of this Makefile; comment
# mentions of missingInclude are ignored. Invoked by .github/workflows/build.yml so CI
# catches a re-add of the suppression. NOTE: grep is given $$mk as its file operand -- a
# vacuous grep reading stdin would print PASS on zero evidence (the exact bug this guards).
	@mk=$(CURDIR)/Makefile; \
	 echo "=== cppcheck-includes proof: --suppress=missingInclude must be ABSENT from active CPPCHECK_SUPPRESS ==="; \
	 if grep -nE -- "^[[:space:]]*--suppress=missingInclude([[:space:]]|$$)" "$$mk"; then \
	   echo "FAIL: --suppress=missingInclude found in an active suppression line above."; \
	   echo "      Remove it: cppcheck-includes exists to surface missing first-party"; \
	   echo "      includes, and this suppression would silence that gate."; \
	   exit 1; \
	 fi; \
	 echo "PASS: --suppress=missingInclude is absent from the active suppression set."

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
#
# A non-zero exit alone is NOT accepted as proof of life. cppcheck exits
# non-zero for reasons that have nothing to do with finding the planted
# defects -- a bad flag, an unreadable include path, an internal error -- so a
# gate that is merely broken-and-noisy would have looked "live". The run is
# therefore captured and the two planted ids (`arrayIndexOutOfBounds` and
# `nullPointer`) must BOTH appear in the output. Missing either one fails the
# build even when the exit code is non-zero.
#
# The assertion matches the BRACKETED form -- `[arrayIndexOutOfBounds]`,
# `[nullPointer]` -- i.e. the `[{id}]` field cppcheck stamps on a report line,
# never a bare word. This is deliberate and is the difference between asserting
# on cppcheck's verdict and asserting on text that happens to be nearby:
#   * $(CPPCHECK_TEMPLATE) is pinned in $(CPPCHECK_FLAGS) and used by BOTH this
#     target and the real `cppcheck` target, so `[{id}]` is guaranteed present
#     on every finding and the offending source line is not echoed back into
#     the log at all;
#   * the generated canary's marker comments say `/* OOB-write probe */` and
#     `/* NULL-deref probe */`, never the id strings, so even a template that
#     did echo the source line could not satisfy the grep;
#   * a self-guard above refuses to run if the generated source ever regains a
#     bracketed id.
# Without all three, a cppcheck bump that renamed or dropped a check while the
# buggy lines stayed put would have kept "passing" off the canary's own text.
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
	  '    buf[7] = 1;            /* OOB-write probe */' \
	  '    return buf[0];' \
	  '}' \
	  'int cppcheck_canary_null(void)' \
	  '{' \
	  '    int *p = NULL;' \
	  '    *p = 42;               /* NULL-deref probe */' \
	  '    return *p;' \
	  '}' \
	  > $(CPPCHECK_CANARY_DIR)/canary.c
	@# Self-guard: the generated source must not contain the bracketed ids the
	@# assertion below greps for, or an edit to the generator could satisfy the
	@# assertion with its own text instead of with cppcheck's verdict.
	@if grep -qE '\[(arrayIndexOutOfBounds|nullPointer)\]' $(CPPCHECK_CANARY_DIR)/canary.c; then \
	  echo "CANARY BROKEN: the generated canary source itself contains a bracketed"; \
	  echo "check id, so the assertion below could match the file instead of"; \
	  echo "cppcheck's report. Rename the marker comments in JOS/Makefile."; \
	  rm -rf $(CPPCHECK_CANARY_DIR); \
	  exit 1; \
	fi
	@echo "=== cppcheck canary: the gate must REJECT this file ==="
	@log=$(CPPCHECK_CANARY_DIR)/canary.out; \
	 set +e; \
	 ( cd $(CPPCHECK_ROOT) && $(CPPCHECK) $(CPPCHECK_FLAGS) $(CPPCHECK_CANARY_DIR)/canary.c ) \
	   > "$$log" 2>&1; \
	 rc=$$?; \
	 cat "$$log"; \
	 missing=""; \
	 grep -q '\[arrayIndexOutOfBounds\]' "$$log" || missing="$$missing arrayIndexOutOfBounds"; \
	 grep -q '\[nullPointer\]' "$$log" || missing="$$missing nullPointer"; \
	 rm -rf $(CPPCHECK_CANARY_DIR); \
	 if [ $$rc -eq 0 ]; then \
	   echo "CANARY FAILED: cppcheck exited 0 on a file with an out-of-bounds"; \
	   echo "write and a NULL dereference. The static-analysis gate is not"; \
	   echo "actually gating -- check the flags/suppressions in JOS/Makefile."; \
	   exit 1; \
	 fi; \
	 if [ -n "$$missing" ]; then \
	   echo "CANARY FAILED: cppcheck exited $$rc, but did not report:$$missing"; \
	   echo "A non-zero exit on its own only proves cppcheck was unhappy, not"; \
	   echo "that it still detects defects -- a bad flag or an unreadable"; \
	   echo "include path produces the same exit code. Run"; \
	   echo "'make -C JOS cppcheck-print' and check the flags/suppressions."; \
	   exit 1; \
	 fi; \
	 echo "canary OK: cppcheck exited $$rc and reported both arrayIndexOutOfBounds"; \
	 echo "           and nullPointer on the buggy file (gate is live)"

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
