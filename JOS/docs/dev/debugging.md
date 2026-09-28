# Debugging

## Common failure: hard fault on Cortex-M4

Most hard faults in FreeRTOS projects come from **NVIC priority vs
`configMAX_SYSCALL_INTERRUPT_PRIORITY`**. Any ISR calling a `...FromISR()`
FreeRTOS API must run at a priority **numerically ≥**
`configMAX_SYSCALL_INTERRUPT_PRIORITY` (set to 5 in this project). Wrong
priority → subtle crashes under load.

## Toolchain strictness mismatch

CI pins Arm GNU Toolchain 14.3.Rel1; a local toolchain of another version
(the Nix shell ships 13, the build container 15.2) can be more or less strict.
Implicit function declarations fail everywhere (`-Werror=implicit-function-declaration`);
always declare prototypes (see `comms.h`). CI is authoritative.

## Tools

- **GDB + OpenOCD + ST-LINK** (or CubeIDE RTOS Viewer for live task view).
- **`arm-none-eabi-size build/JOS.elf`** — budget check (Flash 1 MB / SRAM 320 KB).
- **`build/JOS.map`** — symbol/section analysis.

## LastStates forensics

After an anomaly, dump the LastStates pool (`SEND_DATA` + LastStates alias) and
reconstruct the transition sequence (see `docs/user/README.md`).
