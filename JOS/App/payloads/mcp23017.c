/* mcp23017.c - I2C GPIO expander register driver.
 *
 * Flight backend: I2C4 (PD12/PD13), from the OBC V2.0 gerbers. The 7-bit
 * device address is a caller argument because the strap wiring A0-A2 is not
 * fixed in the delivered corpus. No pin-to-chip-select mapping is invented
 * here; callers pass pin numbers and masks.
 *
 * Host seam: bus primitives go through mcp23017_bus_ops_t, as in temp.c.
 * Transfers are bounded, never infinite waits.
 */

#include "mcp23017.h"

#ifndef HOST_UNIT_TEST
#include "main.h"
#include "stm32l4xx_hal.h"

/* Bounded I2C transfer. HAL_I2C_Mem_* is interrupt-driven; the task still
 * blocks on completion, as the FRAM driver already does on hi2c1. */
#define MCP23017_I2C_TIMEOUT_MS 100u

static int flight_write(uint8_t addr, uint8_t reg, uint8_t value)
{
    I2C_HandleTypeDef *bus = &hi2c4;
    uint16_t dev = (uint16_t)((uint16_t)addr << 1); /* HAL takes 8-bit address */
    HAL_StatusTypeDef st = HAL_I2C_Mem_Write(bus, dev, reg,
                                             I2C_MEMADD_SIZE_8BIT,
                                             &value, 1u,
                                             MCP23017_I2C_TIMEOUT_MS);
    return (st == HAL_OK) ? 0 : -1;
}

static int flight_read(uint8_t addr, uint8_t reg, uint8_t *value)
{
    I2C_HandleTypeDef *bus = &hi2c4;
    uint16_t dev = (uint16_t)((uint16_t)addr << 1);
    HAL_StatusTypeDef st = HAL_I2C_Mem_Read(bus, dev, reg,
                                            I2C_MEMADD_SIZE_8BIT,
                                            value, 1u,
                                            MCP23017_I2C_TIMEOUT_MS);
    return (st == HAL_OK) ? 0 : -1;
}
#else
/* Host build: no HAL. Restoring the flight backend leaves a bus that always
 * refuses, so a test that forgets to inject a model fails closed. */
static int flight_write(uint8_t addr, uint8_t reg, uint8_t value)
{
    (void)addr; (void)reg; (void)value;
    return -1;
}

static int flight_read(uint8_t addr, uint8_t reg, uint8_t *value)
{
    (void)addr; (void)reg; (void)value;
    return -1;
}
#endif

static const mcp23017_bus_ops_t flight_ops = {
    .write = flight_write,
    .read  = flight_read,
};

static const mcp23017_bus_ops_t *g_ops = &flight_ops;

/* 0 means unconfigured; a valid address is never 0. */
static uint8_t s_addr;
static uint16_t s_latch;
/* 0 when the cached latch may not match the hardware: a write failed after the
 * chip may have applied it, so the next set_pin would clobber unknown outputs. */
static int s_latch_valid;

static void forget(void)
{
    s_addr = 0u;
    s_latch = 0u;
    s_latch_valid = 0;
}

#ifdef HOST_UNIT_TEST
void mcp23017_inject_ops(const mcp23017_bus_ops_t *ops)
{
    g_ops = (ops != NULL) ? ops : &flight_ops;
}

void mcp23017_restore_flight_ops(void)
{
    g_ops = &flight_ops;
}
#endif

static int configured(void)
{
    return s_addr != 0u;
}

int mcp23017_init(unsigned int addr)
{
    uint8_t iocon = 0u;
    uint8_t olata = 0u;
    uint8_t olatb = 0u;

    /* A failed init must not leave the previous device callable. */
    forget();
    if ((addr < MCP23017_ADDR_MIN) || (addr > MCP23017_ADDR_MAX)) {
        return -1;
    }
    /* Bank 1 moves every register; this driver implements bank 0 only.
     * IOCON sits at 0x05 in bank 1, while 0x0A is OLATA there and cannot
     * reveal the bank bit. 0x05 is GPINTENB in bank 0 and is zero at
     * power-on, so a set BANK bit there means a bank-1 device. */
    if (g_ops->read((uint8_t)addr, MCP23017_REG_IOCON_BANK1, &iocon) != 0) {
        return -1;
    }
    if ((iocon & MCP23017_IOCON_BANK) != 0u) {
        return -1;
    }
    /* Warm reset keeps the expander's latches while the MCU forgets them.
     * Seed the cache from the hardware instead of assuming zero. */
    if (g_ops->read((uint8_t)addr, MCP23017_REG_OLATA, &olata) != 0) {
        return -1;
    }
    if (g_ops->read((uint8_t)addr, MCP23017_REG_OLATB, &olatb) != 0) {
        return -1;
    }
    s_latch = (uint16_t)((uint16_t)olata | ((uint16_t)olatb << 8));
    s_latch_valid = 1;
    s_addr = (uint8_t)addr;
    return 0;
}

void mcp23017_close(void)
{
    forget();
}

int mcp23017_set_direction(uint16_t mask)
{
    if (!configured()) {
        return -1;
    }
    /* GPA7/GPB7 are output-only on the MCP23017 (DS20001952 Rev D, 3.5.1). */
    if ((mask & (uint16_t)~MCP23017_INPUT_MASK) != 0u) {
        return -1;
    }
    if (g_ops->write(s_addr, MCP23017_REG_IODIRA,
                     (uint8_t)(mask & 0xFFu)) != 0) {
        return -1;
    }
    if (g_ops->write(s_addr, MCP23017_REG_IODIRB,
                     (uint8_t)(mask >> 8)) != 0) {
        return -1;
    }
    return 0;
}

int mcp23017_write_output(uint16_t value)
{
    if (!configured()) {
        return -1;
    }
    if (g_ops->write(s_addr, MCP23017_REG_OLATA,
                     (uint8_t)(value & 0xFFu)) != 0) {
        /* HAL can report an error after the byte reached the expander, so a
         * failed first transfer also leaves the cache unknown. */
        s_latch_valid = 0;
        return -1;
    }
    if (g_ops->write(s_addr, MCP23017_REG_OLATB,
                     (uint8_t)(value >> 8)) != 0) {
        /* OLATA may already hold the new byte while OLATB still holds the old
         * one: the cache no longer describes the hardware. */
        s_latch_valid = 0;
        return -1;
    }
    s_latch = value;
    s_latch_valid = 1;
    return 0;
}

int mcp23017_read_input(uint16_t *value)
{
    uint8_t a = 0u;
    uint8_t b = 0u;

    if ((value == NULL) || !configured()) {
        return -1;
    }
    if (g_ops->read(s_addr, MCP23017_REG_GPIOA, &a) != 0) {
        return -1;
    }
    if (g_ops->read(s_addr, MCP23017_REG_GPIOB, &b) != 0) {
        return -1;
    }
    /* Bits 7 and 15 are output-only pins, never inputs. */
    *value = (uint16_t)(((uint16_t)a | ((uint16_t)b << 8)) & MCP23017_INPUT_MASK);
    return 0;
}

int mcp23017_set_pin(int pin, int high)
{
    uint8_t reg;
    uint8_t value;

    if ((pin < 0) || (pin >= (int)MCP23017_PIN_COUNT) || !configured()) {
        return -1;
    }
    /* After a failed write the cache may not match the hardware; refuse rather
     * than clobber outputs the caller believes are committed. */
    if (!s_latch_valid) {
        return -1;
    }
    if (pin < 8) {
        reg = MCP23017_REG_OLATA;
        value = (uint8_t)s_latch;
        if (high) {
            value = (uint8_t)(value | (uint8_t)(1u << pin));
        } else {
            value = (uint8_t)(value & (uint8_t)~(1u << pin));
        }
    } else {
        reg = MCP23017_REG_OLATB;
        value = (uint8_t)(s_latch >> 8);
        if (high) {
            value = (uint8_t)(value | (uint8_t)(1u << (pin - 8)));
        } else {
            value = (uint8_t)(value & (uint8_t)~(1u << (pin - 8)));
        }
    }
    if (g_ops->write(s_addr, reg, value) != 0) {
        s_latch_valid = 0;  /* the byte may have landed; the cache is unknown */
        return -1;
    }
    if (pin < 8) {
        s_latch = (uint16_t)((s_latch & 0xFF00u) | value);
    } else {
        s_latch = (uint16_t)((s_latch & 0x00FFu) | ((uint16_t)value << 8));
    }
    return 0;
}

int mcp23017_get_pin(int pin, int *high)
{
    uint8_t reg;
    uint8_t value = 0u;

    if ((pin < 0) || (pin >= (int)MCP23017_PIN_COUNT) || (high == NULL) ||
        !configured()) {
        return -1;
    }
    /* GPA7/GPB7 are output-only; they cannot be read as inputs. */
    if ((pin & 7) == 7) {
        return -1;
    }
    reg = (pin < 8) ? MCP23017_REG_GPIOA : MCP23017_REG_GPIOB;
    if (g_ops->read(s_addr, reg, &value) != 0) {
        return -1;
    }
    *high = ((value >> (pin & 7)) & 1u) != 0u ? 1 : 0;
    return 0;
}
