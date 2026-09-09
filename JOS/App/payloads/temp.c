/* temp.c — 1-wire bus driver for the 4x TMP1827 temperature sensors (PB2).
 *
 * Bus ownership: this file is the ONLY owner of PB2. The pin stays in
 * open-drain output mode from MX_GPIO_Init() on; driving = WritePin(RESET),
 * releasing = WritePin(SET) with the external pull-up restoring HIGH, so no
 * per-bit mode switching is needed.
 *
 * Timing: standard speed (TMP1827 datasheet Table 8-1/8-2):
 *   reset low 480..560 us, presence 60..240 us;
 *   write-0 low 60..120 us, write-1 low 2..15 us, slot ~= 70 us + recovery;
 *   read: drive low 2.5..5 us, release, sample ~10 us later, slot ~= 70 us.
 * temp_delay_us() spins on SystemCoreClock (same technique as the RadioLib
 * HAL); only used pre-scheduler / in a low-rate task, never in an ISR.
 *
 * Host seam: all bus primitives go through temp_bus_ops_t. Flight binds the
 * PB2 backend; unit tests inject a scripted multi-device model via
 * temp_inject_ops() (HOST_UNIT_TEST only).
 */

#include "temp.h"
#include "main.h"

#ifdef HOST_UNIT_TEST
/* On the host there is no DWT/SystemCoreClock: the fake bus answers
 * instantly, so the delay is a no-op. */
static void temp_delay_us(uint32_t us) { (void)us; }
#else
#include "stm32l4xx_hal.h"
static void temp_delay_us(uint32_t us)
{
    volatile uint32_t n = (SystemCoreClock / 1000000u) * us / 4u;
    while (n-- != 0u) {
        __NOP();
    }
}
#endif

/* ---------- Bus primitive operations (type in temp.h, HOST seam) ---------- */

/* ----- PB2 flight backend (open-drain, idle HIGH) ----- */

static void pb2_drive(int low)
{
    HAL_GPIO_WritePin(TEMP_1WIRE_GPIO_Port, TEMP_1WIRE_Pin,
                      low ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static int pb2_reset(void)
{
    int presence;
    pb2_drive(1);
    temp_delay_us(500u);          /* tRSTL 480..560 us */
    pb2_drive(0);
    temp_delay_us(70u);           /* tPDH 15..60 us + margin */
    presence = (HAL_GPIO_ReadPin(TEMP_1WIRE_GPIO_Port, TEMP_1WIRE_Pin)
                == GPIO_PIN_RESET);
    temp_delay_us(430u);          /* rest of tRSTH window */
    return presence;
}

static void pb2_write_bit(int bit)
{
    if (bit != 0) {
        pb2_drive(1);
        temp_delay_us(6u);        /* tWR1L 2..15 us */
        pb2_drive(0);
        temp_delay_us(64u);
    } else {
        pb2_drive(1);
        temp_delay_us(65u);       /* tWR0L 60..120 us */
        pb2_drive(0);
        temp_delay_us(5u);        /* tREC >= 2 us */
    }
}

static int pb2_read_bit(void)
{
    int v;
    pb2_drive(1);
    temp_delay_us(3u);            /* tRL 2.5..5 us */
    pb2_drive(0);
    temp_delay_us(10u);           /* sample inside tMSW */
    v = (HAL_GPIO_ReadPin(TEMP_1WIRE_GPIO_Port, TEMP_1WIRE_Pin)
         == GPIO_PIN_SET) ? 1 : 0;
    temp_delay_us(57u);           /* rest of the ~70 us slot */
    return v;
}

static const temp_bus_ops_t pb2_ops = {
    .reset     = pb2_reset,
    .write_bit = pb2_write_bit,
    .read_bit  = pb2_read_bit,
};

static const temp_bus_ops_t *g_ops = &pb2_ops;

#ifdef HOST_UNIT_TEST
/* Unit-test seam: replace the PB2 backend with a scripted bus model. */
void temp_inject_ops(const temp_bus_ops_t *ops)
{
    g_ops = ops;
}

/* Unit-test seam: bind the flight PB2 backend back (see temp.h). */
void temp_restore_flight_ops(void)
{
    g_ops = &pb2_ops;
}
#endif

/* ---------- Byte-level protocol (backend-agnostic) ---------- */

static void bus_write_byte(uint8_t b)
{
    for (uint8_t i = 0u; i < 8u; i++) {
        g_ops->write_bit((b >> i) & 1u);
    }
}

static uint8_t bus_read_byte(void)
{
    uint8_t b = 0u;
    for (uint8_t i = 0u; i < 8u; i++) {
        if (g_ops->read_bit() != 0) {
            b |= (uint8_t)(1u << i);
        }
    }
    return b;
}

/* ---------- Dallas/Maxim CRC-8 (x^8 + x^5 + x^4 + 1, reflected 0x8C) ---------- */

uint8_t tmp1827_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0u;
    for (size_t n = 0u; n < len; n++) {
        uint8_t b = data[n];
        for (uint8_t i = 0u; i < 8u; i++) {
            uint8_t mix = (uint8_t)((crc ^ b) & 1u);
            crc >>= 1;
            if (mix != 0u) {
                crc ^= 0x8Cu;
            }
            b >>= 1;
        }
    }
    return crc;
}

/* ---------- Raw -> 0.1 C (LSB = 7.8125 mC = 5/64 tenth, exact) ---------- */

int16_t tmp1827_raw_to_tenths_c(int16_t raw)
{
    int32_t t = (int32_t)raw * 5;               /* fits: 32767*5 = 163835 */
    t += (t >= 0) ? 32 : -32;                   /* round half away from zero */
    return (int16_t)(t / 64);
}

/* ---------- SEARCHADDR enumeration ---------- */

static uint64_t s_roms[TEMP_SENSOR_COUNT];
static int s_count;

static int search_one(uint64_t *rom_out, int last_discrepancy)
{
    uint64_t rom = 0u;
    int last_zero = 0;
    int bit;

    if (g_ops->reset() == 0) {
        return -1;                              /* nobody on the bus */
    }
    bus_write_byte(TEMP_CMD_SEARCHADDR);
    /* bit is 1-based (Dallas reference numbering): last_discrepancy == 0
     * means "first pass", so every discrepancy takes the 0 branch. */
    for (bit = 1; bit <= 64; bit++) {
        int b0 = g_ops->read_bit();             /* true bit */
        int b1 = g_ops->read_bit();             /* complement */
        int take;
        if ((b0 != 0) && (b1 != 0)) {
            return -1;                          /* no device answered */
        }
        if (b0 != b1) {
            take = b0;                              /* unanimous: follow it */
        } else {
            /* Discrepancy: two devices disagree here. last_zero must track
             * ONLY these branch points (Dallas AN187): recording an
             * agreement-0 past the last real fork would make the next pass
             * retrace this same device forever. */
            if (bit < last_discrepancy) {
                take = (int)((*rom_out >> (bit - 1)) & 1u); /* prev path */
            } else if (bit == last_discrepancy) {
                take = 1;
            } else {
                take = 0;
            }
            if (take == 0) {
                last_zero = bit;
            }
        }
        if (take != 0) {
            rom |= ((uint64_t)1u << (bit - 1));
        }
        g_ops->write_bit(take);
    }
    *rom_out = rom;
    return last_zero;
}

int temp_init(void)
{
    uint64_t rom = 0u;
    int last = 0;
    s_count = 0;

    for (uint8_t dev = 0u; dev < TEMP_SENSOR_COUNT; dev++) {
        uint8_t bytes[TEMP_ROM_SIZE];
        int next;
        uint8_t i;

        next = search_one(&rom, last);
        if (next < 0) {
            break;                              /* bus empty or error */
        }
        for (i = 0u; i < TEMP_ROM_SIZE; i++) {
            bytes[i] = (uint8_t)(rom >> (i * 8u));
        }
        /* Accept only genuine TMP1827 ROMs: family 0x27 + valid CRC. */
        if ((bytes[0] != TEMP_FAMILY_CODE) ||
            (tmp1827_crc8(bytes, 7u) != bytes[7])) {
            break;
        }
        s_roms[s_count++] = rom;
        if (next == 0) {
            break;                              /* last device on the bus */
        }
        last = next;
    }
    return s_count;
}

int temp_sensor_count(void)
{
    return s_count;
}

/* ---------- Convert + read ---------- */

#define TEMP_CONVERT_POLL_MS 100u

static int address_sensor(uint8_t idx)
{
    uint64_t rom;
    int8_t b;
    if ((idx >= TEMP_SENSOR_COUNT) || (idx >= (uint8_t)s_count)) {
        return -1;
    }
    if (g_ops->reset() == 0) {
        return -1;
    }
    rom = s_roms[idx];
    bus_write_byte(TEMP_CMD_MATCHADDR);
    for (b = 0; b < 8; b++) {
        bus_write_byte((uint8_t)(rom >> (b * 8)));
    }
    return 0;
}

int temp_read_tenths_c(uint8_t idx, int16_t *out_tenths_c)
{
    uint8_t lsb, msb;
    uint32_t poll;

    if (out_tenths_c == NULL) {
        return -1;
    }
    if (address_sensor(idx) != 0) {
        return -1;
    }
    bus_write_byte(TEMP_CMD_CONVERT);
    /* Conversion done when a device releases the bus HIGH (tACT <= 6.12 ms
     * single, longer with averaging: poll up to 100 ms). */
    for (poll = 0u; poll < TEMP_CONVERT_POLL_MS; poll++) {
        if (g_ops->read_bit() != 0) {
            break;
        }
        temp_delay_us(1000u);
    }
    if (poll >= TEMP_CONVERT_POLL_MS) {
        return -1;
    }
    if (address_sensor(idx) != 0) {
        return -1;
    }
    bus_write_byte(TEMP_CMD_READ_SP1);
    lsb = bus_read_byte();
    msb = bus_read_byte();
    /* Master may end a scratchpad read with a reset at any byte boundary. */
    (void)g_ops->reset();
    *out_tenths_c = tmp1827_raw_to_tenths_c((int16_t)((uint16_t)lsb |
                                                      ((uint16_t)msb << 8)));
    return 0;
}

int temp_read_all_tenths_c(int16_t *out_tenths_c, size_t n)
{
    int ok = 0;
    size_t i;
    if ((out_tenths_c == NULL) || (n == 0u)) {
        return -1;
    }
    for (i = 0u; (i < n) && (i < (size_t)s_count); i++) {
        if (temp_read_tenths_c((uint8_t)i, &out_tenths_c[i]) == 0) {
            ok++;
        }
    }
    return ok;
}
