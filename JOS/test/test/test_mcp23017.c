/* test_mcp23017.c - behaviour of the MCP23017 register driver.
 *
 * The I2C bus is a scripted model: register values and per-register failure
 * injection. Every test drives the public API only, so a change in register
 * use or in fail-closed behaviour breaks an assertion.
 */
#include "unity.h"
#include "mcp23017.h"
#include <string.h>

#define ADDR 0x20u

static uint8_t regs[256];
static uint8_t fail_read_reg;
static uint8_t fail_write_reg;
static uint8_t last_addr;
static uint8_t last_read_reg;
static uint8_t first_read_reg;
static int reads;
static int writes;

static int fake_write(uint8_t addr, uint8_t reg, uint8_t value)
{
    last_addr = addr;
    writes++;
    if (reg == fail_write_reg) {
        return -1;
    }
    regs[reg] = value;
    return 0;
}

static int fake_read(uint8_t addr, uint8_t reg, uint8_t *value)
{
    last_addr = addr;
    last_read_reg = reg;
    if (reads == 0) {
        first_read_reg = reg;
    }
    reads++;
    if (reg == fail_read_reg) {
        return -1;
    }
    *value = regs[reg];
    return 0;
}

static const mcp23017_bus_ops_t fake_ops = {
    .write = fake_write,
    .read  = fake_read,
};

void setUp(void)
{
    memset(regs, 0, sizeof(regs));
    fail_read_reg = 0xFFu;
    fail_write_reg = 0xFFu;
    last_addr = 0u;
    last_read_reg = 0u;
    first_read_reg = 0u;
    reads = 0;
    writes = 0;
    mcp23017_close();
    mcp23017_inject_ops(&fake_ops);
}

void tearDown(void)
{
    mcp23017_close();
    mcp23017_restore_flight_ops();
}

static void init_ok(void)
{
    TEST_ASSERT_EQUAL_INT(0, mcp23017_init(ADDR));
    reads = 0;
    writes = 0;
}

void test_init_rejects_addresses_outside_strap_range(void)
{
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_init(MCP23017_ADDR_MIN - 1u));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_init(MCP23017_ADDR_MAX + 1u));
    TEST_ASSERT_EQUAL_INT(0, reads);
}

void test_init_probes_iocon_and_accepts_bank_zero(void)
{
    TEST_ASSERT_EQUAL_INT(0, mcp23017_init(0x27u));
    TEST_ASSERT_EQUAL_INT(3, reads);                  /* bank probe + OLATA + OLATB */
    TEST_ASSERT_EQUAL_UINT8(MCP23017_REG_IOCON_BANK1, first_read_reg);
    TEST_ASSERT_EQUAL_UINT8(MCP23017_REG_OLATB, last_read_reg);
    TEST_ASSERT_EQUAL_UINT8(0x27u, last_addr);
}

void test_init_seeds_latch_from_hardware(void)
{
    /* Warm reset: the expander keeps its output latches, the MCU does not. */
    regs[MCP23017_REG_OLATA] = 0xFFu;
    regs[MCP23017_REG_OLATB] = 0xFFu;
    TEST_ASSERT_EQUAL_INT(0, mcp23017_init(ADDR));
    TEST_ASSERT_EQUAL_INT(0, mcp23017_set_pin(0u, 0));
    TEST_ASSERT_EQUAL_UINT8(0xFEu, regs[MCP23017_REG_OLATA]);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, regs[MCP23017_REG_OLATB]);
}

void test_init_fails_when_olat_is_unreadable(void)
{
    fail_read_reg = MCP23017_REG_OLATA;
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_init(ADDR));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_write_output(0u));
    TEST_ASSERT_EQUAL_INT(0, writes);
}

void test_failed_reinit_releases_previous_device(void)
{
    init_ok();
    fail_read_reg = MCP23017_REG_IOCON_BANK1;
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_init(0x21u));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_write_output(0x00FFu));
    TEST_ASSERT_EQUAL_INT(0, writes);
    fail_read_reg = 0xFFu;
    TEST_ASSERT_EQUAL_INT(0, mcp23017_init(0x21u));
    TEST_ASSERT_EQUAL_INT(0, mcp23017_write_output(0x0000u));
    TEST_ASSERT_EQUAL_INT(2, writes);
}

void test_init_rejects_runtime_address_above_uint8(void)
{
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_init(0x120u));
    TEST_ASSERT_EQUAL_INT(0, reads);
}

void test_init_refuses_bank_one_layout(void)
{
    /* Bank 1: IOCON is at 0x05 with BANK set, and 0x0A is OLATA (clear).
     * Reading only 0x0A would wrongly accept this device. */
    regs[MCP23017_REG_IOCON_BANK1] = MCP23017_IOCON_BANK;
    regs[MCP23017_REG_IOCON] = 0u;
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_init(ADDR));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_write_output(0u));
    TEST_ASSERT_EQUAL_INT(0, writes);
}

void test_init_refuses_set_bit_at_bank_one_iocon_alias(void)
{
    /* 0x05 is GPINTENB in bank 0. Only its BANK-bit meaning in bank 1
     * makes it a discriminator, so a set bit here must refuse. */
    regs[MCP23017_REG_IOCON_BANK1] = MCP23017_IOCON_BANK;
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_init(ADDR));
}

void test_init_refuses_missing_device(void)
{
    fail_read_reg = MCP23017_REG_IOCON_BANK1;
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_init(ADDR));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_set_direction(0xFFFFu));
}

void test_operations_refuse_before_init(void)
{
    uint16_t value = 0xABCDu;
    int high = 0;

    TEST_ASSERT_EQUAL_INT(-1, mcp23017_set_direction(0xFFFFu));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_write_output(0xFFFFu));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_read_input(&value));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_set_pin(0u, 1));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_get_pin(0u, &high));
    TEST_ASSERT_EQUAL_INT(0, writes);
    TEST_ASSERT_EQUAL_INT(0, reads);
    TEST_ASSERT_EQUAL_HEX16(0xABCDu, value);
}

void test_direction_writes_both_ports_in_little_endian_order(void)
{
    init_ok();
    TEST_ASSERT_EQUAL_INT(0, mcp23017_set_direction(0x2543u));
    TEST_ASSERT_EQUAL_UINT8(0x43u, regs[MCP23017_REG_IODIRA]);
    TEST_ASSERT_EQUAL_UINT8(0x25u, regs[MCP23017_REG_IODIRB]);
    TEST_ASSERT_EQUAL_UINT8(ADDR, last_addr);
}

void test_direction_propagates_write_failure(void)
{
    init_ok();
    fail_write_reg = MCP23017_REG_IODIRB;
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_set_direction(0x2543u));
    TEST_ASSERT_EQUAL_UINT8(0x43u, regs[MCP23017_REG_IODIRA]);
}

void test_output_latches_and_input_merge_ports(void)
{
    uint16_t value = 0u;

    init_ok();
    TEST_ASSERT_EQUAL_INT(0, mcp23017_write_output(0x1234u));
    TEST_ASSERT_EQUAL_UINT8(0x34u, regs[MCP23017_REG_OLATA]);
    TEST_ASSERT_EQUAL_UINT8(0x12u, regs[MCP23017_REG_OLATB]);

    regs[MCP23017_REG_GPIOA] = 0x78u;
    regs[MCP23017_REG_GPIOB] = 0x56u;
    TEST_ASSERT_EQUAL_INT(0, mcp23017_read_input(&value));
    TEST_ASSERT_EQUAL_HEX16(0x5678u, value);
}

void test_read_input_refuses_null_and_bus_error(void)
{
    uint16_t value = 0xAAAAu;

    init_ok();
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_read_input(NULL));
    fail_read_reg = MCP23017_REG_GPIOA;
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_read_input(&value));
    TEST_ASSERT_EQUAL_HEX16(0xAAAAu, value);

    fail_read_reg = MCP23017_REG_GPIOB;
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_read_input(&value));
    TEST_ASSERT_EQUAL_HEX16(0xAAAAu, value);
}

void test_set_pin_modifies_only_one_latch_byte(void)
{
    init_ok();
    TEST_ASSERT_EQUAL_INT(0, mcp23017_write_output(0x1A34u));
    TEST_ASSERT_EQUAL_INT(0, mcp23017_set_pin(3u, 1));
    TEST_ASSERT_EQUAL_UINT8(0x3Cu, regs[MCP23017_REG_OLATA]);
    TEST_ASSERT_EQUAL_UINT8(0x1Au, regs[MCP23017_REG_OLATB]);

    TEST_ASSERT_EQUAL_INT(0, mcp23017_set_pin(11u, 0));
    TEST_ASSERT_EQUAL_UINT8(0x3Cu, regs[MCP23017_REG_OLATA]);
    TEST_ASSERT_EQUAL_UINT8(0x12u, regs[MCP23017_REG_OLATB]);

    TEST_ASSERT_EQUAL_INT(0, mcp23017_set_pin(3u, 0));
    TEST_ASSERT_EQUAL_INT(0, mcp23017_set_pin(11u, 1));
    TEST_ASSERT_EQUAL_UINT8(0x34u, regs[MCP23017_REG_OLATA]);
    TEST_ASSERT_EQUAL_UINT8(0x1Au, regs[MCP23017_REG_OLATB]);
}

void test_set_pin_rejects_out_of_range_pin(void)
{
    init_ok();
    TEST_ASSERT_EQUAL_INT(0, mcp23017_write_output(0x1234u));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_set_pin(MCP23017_PIN_COUNT, 1));
    TEST_ASSERT_EQUAL_INT(2, writes);
}

void test_set_pin_failure_invalidates_cached_latch(void)
{
    init_ok();
    TEST_ASSERT_EQUAL_INT(0, mcp23017_write_output(0x1234u));
    fail_write_reg = MCP23017_REG_OLATA;
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_set_pin(3u, 1));

    /* The byte may have landed: the cache is unknown, so a later pin write
     * refuses instead of clobbering outputs. */
    fail_write_reg = 0xFFu;
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_set_pin(3u, 1));
    TEST_ASSERT_EQUAL_UINT8(0x34u, regs[MCP23017_REG_OLATA]);

    /* A full write re-establishes a known state. */
    TEST_ASSERT_EQUAL_INT(0, mcp23017_write_output(0x1234u));
    TEST_ASSERT_EQUAL_INT(0, mcp23017_set_pin(3u, 1));
    TEST_ASSERT_EQUAL_UINT8(0x3Cu, regs[MCP23017_REG_OLATA]);
}

void test_get_pin_reads_correct_port_and_bit(void)
{
    int high = -1;

    init_ok();
    regs[MCP23017_REG_GPIOA] = 0x08u;
    regs[MCP23017_REG_GPIOB] = 0x40u;
    TEST_ASSERT_EQUAL_INT(0, mcp23017_get_pin(3u, &high));
    TEST_ASSERT_EQUAL_INT(1, high);
    TEST_ASSERT_EQUAL_INT(0, mcp23017_get_pin(2u, &high));
    TEST_ASSERT_EQUAL_INT(0, high);
    TEST_ASSERT_EQUAL_INT(0, mcp23017_get_pin(14u, &high));
    TEST_ASSERT_EQUAL_INT(1, high);
}

void test_input_only_pins_reject_gpa7_gpb7(void)
{
    int high = 7;

    init_ok();
    /* DS20001952 Rev D: GPA7/GPB7 are output-only on the MCP23017. */
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_set_direction(0x0080u));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_set_direction(0x8000u));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_get_pin(7, &high));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_get_pin(15, &high));
    TEST_ASSERT_EQUAL_INT(7, high);
    TEST_ASSERT_EQUAL_INT(0, writes);

    /* Driving them as outputs stays legal. */
    TEST_ASSERT_EQUAL_INT(0, mcp23017_set_pin(7, 1));
    TEST_ASSERT_EQUAL_INT(0, mcp23017_set_pin(15, 1));
}

void test_read_input_masks_output_only_pins(void)
{
    uint16_t value = 0u;

    init_ok();
    regs[MCP23017_REG_GPIOA] = 0xFFu;
    regs[MCP23017_REG_GPIOB] = 0xFFu;
    TEST_ASSERT_EQUAL_INT(0, mcp23017_read_input(&value));
    TEST_ASSERT_EQUAL_HEX16(MCP23017_INPUT_MASK, value);
}

void test_partial_output_write_invalidates_latch_cache(void)
{
    TEST_ASSERT_EQUAL_INT(0, mcp23017_init(ADDR));
    TEST_ASSERT_EQUAL_INT(0, mcp23017_write_output(0x1234u));

    fail_write_reg = MCP23017_REG_OLATB;
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_write_output(0x00FFu));
    fail_write_reg = 0xFFu;

    /* OLATA now holds 0xFF while the cache says 0x34: the cache is unknown. */
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_set_pin(0u, 1));
    TEST_ASSERT_EQUAL_UINT8(0xFFu, regs[MCP23017_REG_OLATA]);

    /* A full write re-establishes a known state. */
    TEST_ASSERT_EQUAL_INT(0, mcp23017_write_output(0x0000u));
    TEST_ASSERT_EQUAL_INT(0, mcp23017_set_pin(0u, 1));
    TEST_ASSERT_EQUAL_UINT8(0x01u, regs[MCP23017_REG_OLATA]);
}

void test_pin_arguments_are_not_truncated(void)
{
    int high = 7;

    init_ok();
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_set_pin(256, 1));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_get_pin(256, &high));
    TEST_ASSERT_EQUAL_INT(7, high);
    TEST_ASSERT_EQUAL_INT(0, writes);
}

void test_get_pin_refuses_bad_arguments_and_bus_error(void)
{
    int high = 7;

    init_ok();
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_get_pin(MCP23017_PIN_COUNT, &high));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_get_pin(0u, NULL));
    fail_read_reg = MCP23017_REG_GPIOB;
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_get_pin(8u, &high));
    TEST_ASSERT_EQUAL_INT(7, high);
}

void test_close_disables_all_operations(void)
{
    uint16_t value = 0u;

    init_ok();
    mcp23017_close();
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_write_output(0xFFFFu));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_read_input(&value));
    TEST_ASSERT_EQUAL_INT(0, writes);
    TEST_ASSERT_EQUAL_INT(0, reads);
}

void test_flight_backend_fails_closed_in_host_build(void)
{
    mcp23017_restore_flight_ops();
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_init(ADDR));
    TEST_ASSERT_EQUAL_INT(-1, mcp23017_write_output(0xFFFFu));
}
