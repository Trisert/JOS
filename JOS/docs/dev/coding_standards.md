# Coding Standards

## Language

- **Primary:** C11 (gnu11)
- **Secondary:** C++14 (gnu++14) for RadioLib

## Compiler Flags

```makefile
CFLAGS := -std=gnu11 -g3 -O0
CXXFLAGS := -std=gnu++14 -g3 -O0
```

## MISRA Compliance

This project aims to comply with MISRA C:2012 guidelines for safety-critical flight software.

### Key Rules Applied

| Rule | Description |
|------|-------------|
| R.10.1 | Operands shall not be of an effectively boolean type |
| R.11.3 | Cast should not be performed between pointer type and non-pointer type |
| R.13.1 | Boolean expressions shall not be used with bitwise operators |
| R.14.1 | Non-zero should not be used as the right operand of logical AND/OR |
| R.15.1 | Avoid side effects in assertions |
| R.16.1 | A switch statement shall be well-formed |
| R.16.2 | A switch expression shall not be an incomplete enum type |
| R.16.7 | The switch expression shall not be constant |
| R.17.1 | No if-else chains without braces |
| R.17.8 | Avoid function calls in if/for/while conditions |
| R.21.1 | Avoid #define'd macros used as constants |

### Static Analysis

Run with MISRA checking enabled:

```bash
make CFLAGS="-std=gnu11 -misra"
```

(Not currently enabled - to be added)

## Naming Conventions

### Files
- Lowercase with underscore: `state_machine.c`, `state_machine.h`

### Functions
- Lowercase with underscore: `state_machine_init()`, `watchdog_register_task()`

### Types/Structs
- Suffix `_t` for types: `obw_state_t`, `bms_status_t`

### Enums
- Prefix with module name: `STATE_OFF`, `TRIGGER_BOOT`

### Constants
- UPPER_CASE: `WDG_MAX_TASKS`, `BEACON_INTERVAL_CRIT`

### Macros
- UPPER_CASE: `#define FRAM_READ 0x03`

## Code Style

### Indentation
- 4 spaces (no tabs)

### Braces
- Same line for function definitions
- New line for control statements

```c
// Function
void function_name(void)
{
    if (condition) {
        // code
    }
}
```

### Line Length
- Maximum 100 characters

### Comments
- Use `/** */` for API documentation (Doxygen-style)
- Use `/* */` for implementation comments
- Avoid `//` in C code

### Includes
- System first, then project headers
- Sort alphabetically within groups

```c
#include <stdint.h>
#include "state_machine.h"
#include "obsw_types.h"
```

## Error Handling

- Return `int` with 0 = success, -1 = error
- Check return values
- No exceptions in C

## Memory

- No dynamic allocation (static allocation only)
- Use fixed-size buffers
- Check bounds on array access

## Thread Safety

- Use FreeRTOS mutexes for shared data
- Always acquire mutex before accessing shared state
- Release mutex immediately after access

## Testing

Unit tests should:
- Test public API only
- Use mock objects for hardware
- Verify return values and state changes
- Run on development board before commit