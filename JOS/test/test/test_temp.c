/**
 * @file    test_temp.c
 * @brief   Unit tests for the 4x TMP1827 1-wire driver on PB2
 *          (App/payloads/temp.c, SPF v3 3.7.5.3.1 p.95).
 *
 * The driver talks to the bus only through temp_bus_ops_t, so the four
 * sensors are doubled by a faithful bit-level slave emulator below: it runs
 * the real Dallas SEARCHADDR triplet exchange (AND of the true bit and its
 * complement over the still-participating devices, dropout on choice
 * mismatch), MATCHADDR selection, conversion polling and scratchpad readout.
 * Enumeration order, dropout and CRC/family validation are therefore
 * exercised exactly as on the wire.
 *
 * What is verified
 *   - CRC-8/Maxim check vector ("123456789" -> 0xA1) and ROM residual == 0.
 *   - raw -> 0.1 C conversion (LSB = 7.8125 mC, exact 5/64 tenth).
 *   - temp_init() enumerates all 4 sensors; count accessor agrees.
 *   - per-sensor and read-all temperature reads return the programmed values.
 *   - empty bus -> 0 sensors (boot continues); bad index / NULL -> error.
 *   - non-TMP1827 ROMs (wrong family, bad CRC) are rejected.
 *   - a stuck conversion (bus never releases) times out instead of hanging.
 */

#include "unity.h"
#include "temp.h"
#include "host_support.h"     /* host_gpio_* for the flight-backend test */

#include <string.h>

/* ---------------- Slave emulator (injected via temp_inject_ops) ---------- */

#define EMU_MAX 4

static uint64_t emu_rom[EMU_MAX];
static int16_t  emu_raw[EMU_MAX];
static int      emu_ndev = EMU_MAX;
static int      emu_present = 1;    /* reset() presence pulse */
static int      emu_stuck = 0;      /* conversion never completes */
static int      emu_corrupt = -1;   /* index whose ROM CRC byte is flipped */

typedef enum {
    E_CMD,      /* accumulating a command byte (after reset or MATCH) */
    E_SEARCH0,  /* next read_bit returns the AND of the true bits */
    E_SEARCH1,  /* next read_bit returns the AND of the complements */
    E_SEARCHW,  /* next write_bit is the master's branch choice */
    E_MATCH,    /* accumulating the 8 MATCHADDR ROM bytes */
    E_CONVERT,  /* conversion poll: read_bit returns done (or 0 if stuck) */
    E_READSP    /* shifting out the scratchpad LSB-first */
} emu_phase_t;

static emu_phase_t emu_ph;
static uint8_t  emu_cmd;
static int      emu_cmd_bits;
static int      emu_bit;            /* SEARCHADDR bit position, 1..64 */
static uint64_t emu_active;         /* still-participating devices */
static uint8_t  emu_match[8];
static int      emu_match_n;
static uint64_t emu_selected;
static uint8_t  emu_sp[4];
static int      emu_sp_bit;         /* bit index into emu_sp stream */

static uint64_t emu_rom_at(int i)
{
    uint64_t r = emu_rom[i];
    if (i == emu_corrupt) {
        r ^= ((uint64_t)1u << 60);  /* flip a bit inside the CRC byte */
    }
    return r;
}

static int emu_reset(void)
{
    emu_ph = E_CMD;
    emu_cmd = 0u;
    emu_cmd_bits = 0;
    return emu_present;
}

static void emu_cmd_done(void)
{
    int i;
    switch (emu_cmd) {
    case TEMP_CMD_SEARCHADDR:
        emu_ph = E_SEARCH0;
        emu_bit = 1;
        emu_active = (emu_ndev >= 64) ? ~(uint64_t)0u
                                      : (((uint64_t)1u << emu_ndev) - 1u);
        break;
    case TEMP_CMD_MATCHADDR:
        emu_ph = E_MATCH;
        emu_match_n = 0;
        emu_sp_bit = 0;
        memset(emu_match, 0, sizeof(emu_match));
        break;
    case TEMP_CMD_CONVERT:
        emu_ph = E_CONVERT;
        break;
    case TEMP_CMD_READ_SP1:
        /* Scratchpad of the selected device: temp LSB/MSB + padding. */
        emu_sp[0] = 0u;
        emu_sp[1] = 0u;
        emu_sp[2] = 0u;
        emu_sp[3] = 0u;
        for (i = 0; i < emu_ndev; i++) {
            if (emu_rom_at(i) == emu_selected) {
                emu_sp[0] = (uint8_t)emu_raw[i];
                emu_sp[1] = (uint8_t)((uint16_t)emu_raw[i] >> 8);
                break;
            }
        }
        emu_ph = E_READSP;
        emu_sp_bit = 0;
        break;
    default:
        break;
    }
}

static void emu_write_bit(int bit)
{
    int i;
    switch (emu_ph) {
    case E_CMD:
        if (bit != 0) {
            emu_cmd |= (uint8_t)(1u << emu_cmd_bits);
        }
        if (++emu_cmd_bits >= 8) {
            emu_cmd_done();
        }
        break;
    case E_SEARCHW:
        for (i = 0; i < emu_ndev; i++) {
            if ((emu_active & ((uint64_t)1u << i)) != 0u) {
                int rb = (int)((emu_rom_at(i) >> (emu_bit - 1)) & 1u);
                if (rb != (bit != 0)) {
                    emu_active &= ~((uint64_t)1u << i);
                }
            }
        }
        if (++emu_bit > 64) {
            emu_ph = E_CMD;   /* pass over; master resets for the next one */
            emu_cmd = 0u;
            emu_cmd_bits = 0;
        } else {
            emu_ph = E_SEARCH0;
        }
        break;
    case E_MATCH:
        if (bit != 0) {
            emu_match[emu_match_n] |= (uint8_t)(1u << (emu_sp_bit % 8));
        }
        emu_sp_bit++;
        if (emu_sp_bit >= 8) {
            emu_match_n++;
            emu_sp_bit = 0;
            if (emu_match_n >= 8) {
                uint64_t r = 0u;
                int b;
                for (b = 0; b < 8; b++) {
                    r |= ((uint64_t)emu_match[b]) << (b * 8);
                }
                emu_selected = r;
                emu_ph = E_CMD;   /* CONVERT / READ_SP1 follows, no reset */
                emu_cmd = 0u;
                emu_cmd_bits = 0;
            }
        }
        break;
    default:
        break;
    }
}

static int emu_read_bit(void)
{
    int i, b0;
    switch (emu_ph) {
    case E_SEARCH0:
        b0 = 1;
        for (i = 0; i < emu_ndev; i++) {
            if ((emu_active & ((uint64_t)1u << i)) != 0u) {
                if (((emu_rom_at(i) >> (emu_bit - 1)) & 1u) == 0u) {
                    b0 = 0;
                    break;
                }
            }
        }
        if (emu_active == 0u) {
            b0 = 1;   /* nobody answers: both slots read HIGH */
        }
        emu_ph = E_SEARCH1;
        return b0;
    case E_SEARCH1: {
        int b1 = 1;
        for (i = 0; i < emu_ndev; i++) {
            if ((emu_active & ((uint64_t)1u << i)) != 0u) {
                if (((emu_rom_at(i) >> (emu_bit - 1)) & 1u) != 0u) {
                    b1 = 0;
                    break;
                }
            }
        }
        if (emu_active == 0u) {
            b1 = 1;
        }
        emu_ph = E_SEARCHW;
        return b1;
    }
    case E_CONVERT:
        return emu_stuck ? 0 : 1;
    case E_READSP: {
        int byte = emu_sp_bit / 8;
        int bit = emu_sp_bit % 8;
        int v = 0;
        if (byte < (int)sizeof(emu_sp)) {
            v = (emu_sp[byte] >> bit) & 1;
        }
        emu_sp_bit++;
        return v;
    }
    default:
        return 1;
    }
}

static const temp_bus_ops_t emu_ops = {
    .reset     = emu_reset,
    .write_bit = emu_write_bit,
    .read_bit  = emu_read_bit,
};

/* Empty bus: nobody answers the reset. */
static int empty_reset(void) { return 0; }
static void empty_write(int b) { (void)b; }
static int empty_read(void) { return 1; }
static const temp_bus_ops_t empty_ops = {
    .reset     = empty_reset,
    .write_bit = empty_write,
    .read_bit  = empty_read,
};

/* Build N distinct, valid TMP1827 ROMs (family 0x27 + real CRC). */
static void emu_build_roms(int n)
{
    int i, b;
    emu_ndev = n;
    for (i = 0; i < n; i++) {
        uint8_t bytes[TEMP_ROM_SIZE];
        uint64_t r = 0u;
        bytes[0] = TEMP_FAMILY_CODE;
        bytes[1] = (uint8_t)(0x10u + (uint8_t)i);
        bytes[2] = (uint8_t)(0xA0u + (uint8_t)(i * 37));
        bytes[3] = (uint8_t)(0x55u ^ (uint8_t)(i * 91));
        bytes[4] = (uint8_t)(0x01u + (uint8_t)i);
        bytes[5] = (uint8_t)(0xC3u - (uint8_t)(i * 17));
        bytes[6] = (uint8_t)(0x7Eu + (uint8_t)(i * 5));
        bytes[7] = tmp1827_crc8(bytes, 7u);
        for (b = 0; b < 8; b++) {
            r |= ((uint64_t)bytes[b]) << (b * 8);
        }
        emu_rom[i] = r;
    }
}

void setUp(void)
{
    emu_present = 1;
    emu_stuck = 0;
    emu_corrupt = -1;
    emu_build_roms(EMU_MAX);
    emu_raw[0] = 3200;    /*  25.0 C */
    emu_raw[1] = 1600;    /*  12.5 C */
    emu_raw[2] = 0;       /*   0.0 C */
    emu_raw[3] = -128;    /*  -1.0 C */
    temp_inject_ops(&emu_ops);
}

void tearDown(void)
{
    temp_inject_ops(&emu_ops);
}

/* ---------- Pure helpers ---------- */

/* Published check vector for CRC-8/Maxim (poly 0x31 reflected, init 0). */
void test_crc8_check_vector(void)
{
    static const uint8_t msg[] = "123456789";
    TEST_ASSERT_EQUAL_HEX8(0xA1u, tmp1827_crc8(msg, sizeof(msg) - 1u));
    TEST_ASSERT_EQUAL_HEX8(0x00u, tmp1827_crc8(NULL, 0u));
}

/* A complete ROM (7 bytes + its CRC) has residual 0. */
void test_crc8_rom_residual(void)
{
    uint8_t bytes[TEMP_ROM_SIZE];
    int b;
    for (b = 0; b < 7; b++) {
        bytes[b] = (uint8_t)(emu_rom[0] >> (b * 8));
    }
    bytes[7] = tmp1827_crc8(bytes, 7u);
    TEST_ASSERT_EQUAL_HEX8(0x00u, tmp1827_crc8(bytes, 8u));
}

/* LSB = 7.8125 mC = exactly 5/64 of 0.1 C. */
void test_raw_to_tenths(void)
{
    TEST_ASSERT_EQUAL_INT16(0, tmp1827_raw_to_tenths_c(0));
    TEST_ASSERT_EQUAL_INT16(10, tmp1827_raw_to_tenths_c(128));    /* 1 C */
    TEST_ASSERT_EQUAL_INT16(-10, tmp1827_raw_to_tenths_c(-128));
    TEST_ASSERT_EQUAL_INT16(250, tmp1827_raw_to_tenths_c(3200));  /* 25 C */
    TEST_ASSERT_EQUAL_INT16(125, tmp1827_raw_to_tenths_c(1600));  /* 12.5 C */
    TEST_ASSERT_EQUAL_INT16(1, tmp1827_raw_to_tenths_c(16));      /* rounds */
    TEST_ASSERT_EQUAL_INT16(-1, tmp1827_raw_to_tenths_c(-8));
}

/* ---------- Enumeration ---------- */

void test_init_enumerates_four(void)
{
    TEST_ASSERT_EQUAL_INT(4, temp_init());
    TEST_ASSERT_EQUAL_INT(4, temp_sensor_count());
}

void test_init_empty_bus_returns_zero(void)
{
    temp_inject_ops(&empty_ops);
    TEST_ASSERT_EQUAL_INT(0, temp_init());
    TEST_ASSERT_EQUAL_INT(0, temp_sensor_count());
}

/* A ROM with the wrong family code is not a TMP1827: rejected. */
void test_init_rejects_wrong_family(void)
{
    emu_build_roms(1);
    emu_rom[0] &= ~((uint64_t)0xFFu);
    emu_rom[0] |= 0x28u;   /* another Dallas family, CRC now also wrong */
    TEST_ASSERT_EQUAL_INT(0, temp_init());
}

/* Right family but corrupt CRC: rejected. */
void test_init_rejects_bad_crc(void)
{
    emu_build_roms(1);
    emu_corrupt = 0;
    TEST_ASSERT_EQUAL_INT(0, temp_init());
}

/* ---------- Reads ---------- */

static int contains(int16_t *vals, int n, int16_t want)
{
    int i;
    for (i = 0; i < n; i++) {
        if (vals[i] == want) {
            return 1;
        }
    }
    return 0;
}

void test_read_all_returns_programmed_values(void)
{
    int16_t got[4] = {0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF};
    int ok;

    TEST_ASSERT_EQUAL_INT(4, temp_init());
    ok = temp_read_all_tenths_c(got, 4);
    TEST_ASSERT_EQUAL_INT(4, ok);
    /* Enumeration order follows the ROM bit paths, so compare as a set. */
    TEST_ASSERT_TRUE(contains(got, 4, 250));
    TEST_ASSERT_TRUE(contains(got, 4, 125));
    TEST_ASSERT_TRUE(contains(got, 4, 0));
    TEST_ASSERT_TRUE(contains(got, 4, -10));
}

void test_read_single_sensor(void)
{
    int16_t v = 0x7FFF;
    int i, seen = 0;

    TEST_ASSERT_EQUAL_INT(4, temp_init());
    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(0, temp_read_tenths_c((uint8_t)i, &v));
        if ((v == 250) || (v == 125) || (v == 0) || (v == -10)) {
            seen++;
        }
    }
    TEST_ASSERT_EQUAL_INT(4, seen);
}

void test_read_bad_args(void)
{
    int16_t v = 0;

    TEST_ASSERT_EQUAL_INT(4, temp_init());
    TEST_ASSERT_TRUE(temp_read_tenths_c(4u, &v) < 0);    /* past the end */
    TEST_ASSERT_TRUE(temp_read_tenths_c(0u, NULL) < 0);  /* no output */
    TEST_ASSERT_TRUE(temp_read_all_tenths_c(NULL, 4u) < 0);
    TEST_ASSERT_TRUE(temp_read_all_tenths_c(&v, 0u) < 0);
    /* Untouched slots keep their value on total failure. */
    temp_inject_ops(&empty_ops);
    v = 0x1234;
    TEST_ASSERT_TRUE(temp_read_all_tenths_c(&v, 1u) <= 0);
}

/* A conversion that never completes must time out, not hang the caller. */
void test_read_stuck_conversion_times_out(void)
{
    int16_t v = 0;

    TEST_ASSERT_EQUAL_INT(4, temp_init());
    emu_stuck = 1;
    TEST_ASSERT_TRUE(temp_read_tenths_c(0u, &v) < 0);
}

/* ---------- Flight PB2 backend (via the HAL GPIO doubles) ---------- */

/* The flight backend (pb2_reset/write_bit/read_bit) runs the same SEARCHADDR
 * exchange through the real HAL GPIO doubles with PB2 forced to a constant 0
 * (no slave, no pulse): the forced LOW reads as presence, then constant-0
 * levels trace the all-zero ROM path which the family check rejects.
 * What this pins is that the backend runs end to end without a slave. */
void test_flight_backend_search_runs_on_gpio_doubles(void)
{
    temp_restore_flight_ops();
    host_gpio_reset();
    host_gpio_force_input(2, 0);   /* PB2 constant 0: presence + all-zero ROM */
    TEST_ASSERT_EQUAL_INT(0, temp_init());
    TEST_ASSERT_EQUAL_INT(0, temp_sensor_count());
}

/* Reset answers, but every bus level reads HIGH: the SEARCHADDR triplet
 * sees (1,1) — no device answered mid-search — and enumeration stops. */
static int high_reset(void) { return 1; }
static void high_write(int b) { (void)b; }
static int high_read(void) { return 1; }
static const temp_bus_ops_t high_ops = {
    .reset     = high_reset,
    .write_bit = high_write,
    .read_bit  = high_read,
};

void test_init_rejects_no_device_triplet(void)
{
    temp_inject_ops(&high_ops);
    TEST_ASSERT_EQUAL_INT(0, temp_init());
    TEST_ASSERT_EQUAL_INT(0, temp_sensor_count());
}

/* ---------- Mid-read bus loss ---------- */

/* Same emulator underneath, but the reset line drops on one chosen call:
 * lets the second address_sensor() in temp_read_tenths_c() fail after a
 * good enumeration, CONVERT and poll. */
static int wrap_resets;
static int wrap_fail_at = -1;   /* 1-based reset call to fail, -1 = never */

static int wrap_reset(void)
{
    wrap_resets++;
    if (wrap_resets == wrap_fail_at) {
        return 0;
    }
    return emu_reset();
}

static void wrap_write(int b) { emu_write_bit(b); }
static int wrap_read(void) { return emu_read_bit(); }

static const temp_bus_ops_t wrap_ops = {
    .reset     = wrap_reset,
    .write_bit = wrap_write,
    .read_bit  = wrap_read,
};

void test_read_fails_when_second_address_reset_lost(void)
{
    int16_t v = 0x7FFF;

    temp_inject_ops(&wrap_ops);
    wrap_resets = 0;
    wrap_fail_at = -1;
    TEST_ASSERT_EQUAL_INT(4, temp_init());
    /* Enumeration spent one reset per device; the read below spends one
     * reset per address_sensor(): fail the second. */
    wrap_fail_at = wrap_resets + 2;
    TEST_ASSERT_TRUE(temp_read_tenths_c(0u, &v) < 0);
}
