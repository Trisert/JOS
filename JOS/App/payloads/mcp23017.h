/* mcp23017.h - I2C GPIO expander used for payload/subsystem chip-selects.
 *
 * The MCP23017 datasheet fixes the register map (bank 0, the power-on
 * default). The I2C address is set by straps A0-A2 and is NOT fixed by the
 * shipped corpus: the caller must supply a validated 7-bit address, and
 * the driver refuses to operate until then (AGENTS.md section 1 and 4).
 *
 * This module owns the expander register state. The mapping from its 16
 * output pins to individual chip-selects is an ICD decision and is kept
 * outside this driver.
 */
#ifndef MCP23017_H
#define MCP23017_H

#include <stddef.h>
#include <stdint.h>

/* 7-bit I2C address range selected by straps A0-A2 (DS20001952 Rev D, 3.1). */
#define MCP23017_ADDR_MIN 0x20u
#define MCP23017_ADDR_MAX 0x27u

/* GPA7 and GPB7 are output-only on the MCP23017 (DS20001952 Rev D, Table 2-1
 * and 3.5.1); IO7 must stay an output. Reading them as inputs can corrupt SDA. */
#define MCP23017_INPUT_MASK 0x7F7Fu

/* Bank 0 register addresses, power-on default (MCP23017 datasheet). */
#define MCP23017_REG_IODIRA 0x00u
#define MCP23017_REG_IODIRB 0x01u
#define MCP23017_REG_GPIOA  0x12u
#define MCP23017_REG_GPIOB  0x13u
#define MCP23017_REG_OLATA  0x14u
#define MCP23017_REG_OLATB  0x15u
#define MCP23017_REG_IOCON  0x0Au
/* BANK=1 moves IOCON to 0x05/0x15 (MCP23017 datasheet, register map).
 * Reading 0x05 is a read-only way to tell the two maps apart: it is IOCON
 * with BANK=1, and GPINTENB (zero at power-on) with BANK=0. Reading 0x0A
 * cannot distinguish them because it is OLATA in bank 1. */
#define MCP23017_REG_IOCON_BANK1 0x05u

#define MCP23017_PIN_COUNT 16u

/* IOCON.BANK = 1 moves the registers into bank 1; that layout is not
 * supported here, so init refuses such a device. */
#define MCP23017_IOCON_BANK 0x80u

/* Bus primitive. Returns 0 on success, negative on failure.
 * addr is the 7-bit address; the flight backend shifts it for HAL. */
typedef int (*mcp23017_write_fn)(uint8_t addr, uint8_t reg, uint8_t value);
typedef int (*mcp23017_read_fn)(uint8_t addr, uint8_t reg, uint8_t *value);

typedef struct {
    mcp23017_write_fn write;
    mcp23017_read_fn read;
} mcp23017_bus_ops_t;

/* Probe the device, seed the output cache from OLAT, and latch its address.
 * Refuses an out-of-range address (no truncation), a missing device, a bank-1
 * device or an unreadable OLAT. A failed init leaves the driver deconfigured:
 * earlier address and cache are dropped before any probe runs. */
int mcp23017_init(unsigned int addr);

/* Forget the address and the cached latch. Every operation refuses until init
 * succeeds again. */
void mcp23017_close(void);

/* 1 = input, 0 = output, bit n = pin n. Refuses an input request on GPA7/GPB7,
 * which are output-only. */
int mcp23017_set_direction(uint16_t mask);

/* Write both output latches (bit n = pin n). A failed byte marks the cache
 * unknown, so set_pin refuses until a full write or init succeeds. */
int mcp23017_write_output(uint16_t value);

/* Read both GPIO ports (bit n = pin n). Bits 7 and 15 are output-only and read
 * as 0. */
int mcp23017_read_input(uint16_t *value);

/* Modify one output pin without disturbing the other latch byte. Refuses when
 * the cache is unknown (a previous failed write). */
int mcp23017_set_pin(int pin, int high);

/* Read one input pin; *high is written only on success. Refuses GPA7/GPB7. */
int mcp23017_get_pin(int pin, int *high);

#ifdef HOST_UNIT_TEST
/* Replace the I2C4 HAL backend with a scripted bus model. */
void mcp23017_inject_ops(const mcp23017_bus_ops_t *ops);
void mcp23017_restore_flight_ops(void);
#endif

#endif /* MCP23017_H */
