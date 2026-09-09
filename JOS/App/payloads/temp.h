#ifndef TEMP_H
#define TEMP_H

#include <stdint.h>
#include <stddef.h>

/* ---------- Wiring (OBC V2.0 netlist, SPF v3 3.7.5.3.1 p.95) ----------
 * 4x TMP1827 (TMP1..TMP4, SDQ pins) share ONE 1-wire bus on net
 * TEMP_GPIO_IN -> MCU PB2 (pin 37). Single-bus multi-drop: all four sensors
 * are enumerated at init via SEARCHADDR and addressed with MATCHADDR.
 * Port/pin come from Core/Inc/main.h (CubeMX user defines). */

#define TEMP_SENSOR_COUNT   4u
#define TEMP_ROM_SIZE       8u

/* TMP1827 1-wire protocol (TI datasheet, 1-Wire compatible):
 * family code 0x27, 16-bit two's-complement temperature, LSB = 7.8125 mC
 * (high-precision format, power-on default). Scratchpad-1 byte 0/1 hold the
 * temperature LSB/MSB. */
#define TEMP_FAMILY_CODE    0x27u
#define TEMP_CMD_READADDR   0x33u
#define TEMP_CMD_MATCHADDR  0x55u
#define TEMP_CMD_SEARCHADDR 0xF0u
#define TEMP_CMD_SKIPADDR   0xCCu
#define TEMP_CMD_CONVERT    0x44u
#define TEMP_CMD_READ_SP1   0xBEu

/* Enumerate the bus (SEARCHADDR). Returns the number of TMP1827 sensors
 * found (0..TEMP_SENSOR_COUNT); negative on bus error (no presence pulse).
 * Safe to call before the scheduler starts; blocks for a few ms. A zero
 * return is NOT fatal: the caller keeps booting without temperature data. */
int temp_init(void);

/* Number of sensors found by the last temp_init(). */
int temp_sensor_count(void);

/* Read one sensor (0 = first ROM found). Output in 0.1 C (e.g. 215 = 21.5 C).
 * Returns 0 on success, negative on bus error or bad index. Blocks up to
 * ~100 ms worst case (conversion poll). Call only from a task context,
 * never from an ISR. */
int temp_read_tenths_c(uint8_t idx, int16_t *out_tenths_c);

/* Convert + read every enumerated sensor (up to n slots). Returns the number
 * of successful reads, negative on bus error. Unread slots are untouched. */
int temp_read_all_tenths_c(int16_t *out_tenths_c, size_t n);

/* ---------- Pure protocol helpers (no HAL; host-testable) ---------- */

/* Dallas/Maxim CRC-8 (poly 0x31 reflected): seed 0 over data[0..len). Used
 * for both the 64-bit ROM (byte 7) and scratchpad validation. */
uint8_t tmp1827_crc8(const uint8_t *data, size_t len);

/* Raw 16-bit TMP1827 reading -> 0.1 C, rounded. Exact (LSB = 5/64 tenth):
 * no overflow for the full +/-256 C range. */
int16_t tmp1827_raw_to_tenths_c(int16_t raw);

/* Bus primitives: the byte-level protocol (SEARCHADDR enumeration, MATCH +
 * CONVERT + READ_SP1) runs on reset/write_bit/read_bit. Flight binds the PB2
 * backend below; unit tests inject a scripted multi-device model. */
typedef struct {
    /* Reset + presence detect. Returns 1 when >=1 device answered. */
    int (*reset)(void);
    void (*write_bit)(int bit);
    int (*read_bit)(void);
} temp_bus_ops_t;

#ifdef HOST_UNIT_TEST
/* Unit-test seam: replace the PB2 backend (default) with a scripted model. */
void temp_inject_ops(const temp_bus_ops_t *ops);

/* Unit-test seam: bind the flight PB2 backend back after temp_inject_ops().
 * Lets a test exercise the flight PB2 backend (via temp_init/read) through
 * the HAL GPIO doubles (fakes/main.h + support/hal_stubs.c). */
void temp_restore_flight_ops(void);
#endif

#endif /* TEMP_H */
