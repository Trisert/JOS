#ifndef BEACON_H
#define BEACON_H

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Housekeeping (HB) beacon - byte-exact wire layout
 *
 * Source of the layout: SW_DATA_TYPES.xlsx, sheet "HB" (SYS / SW data
 * management), cross-checked against sheet "Data Types" (field widths and
 * "HB?" flags). That document is a sub-team working doc with NO release
 * status and several cells marked "TBD" / "Da valutare": it is the best
 * available evidence, NOT an approved interface contract. Everything this
 * module does that the document does not state is called out below and in the
 * PR body so it can be confirmed before it is treated as frozen.
 *
 * The HB sheet is an 8-row table, 8 bytes per row (columns "Byte 1".."Byte 8").
 * Rows 2, 3 and 4 are blank (reserved). The full beacon is therefore 8 rows x
 * 8 bytes = 64 bytes:
 *
 *   Row 1  STATUS | I_BAT_CHG | I_BAT_DSG | SC_CHG | V_BAT | BAT_SOC | PAYLOADS | (rsv)
 *   Row 2  (reserved)
 *   Row 3  (reserved)
 *   Row 4  (reserved)
 *   Row 5  GPS_TIMESTAMP (32 bit, spans bytes 1-4 - merged cell C10:F10)
 *   Row 6  TEMP_1 | TEMP_2 | TEMP_3 | TEMP_4 | TEMP_EXT_1 | TEMP_EXT_2 | TEMP_EXT_3 | TEMP_EXT_4
 *   Row 7  TEMP_EXT_5 | TEMP_EXT_6 | BAT_TEMP | CRY_TEMP | GPS_X (2B) | GPS_Y (2B)
 *   Row 8  GPS_Z (2B) | GPS_X_DOT (2B) | GPS_Y_DOT (2B) | GPS_Z_DOT (2B)
 *
 * IMPORTANT (width correction vs the original task brief): the six GPS
 * components in rows 7-8 are EACH TWO BYTES, not one. The HB sheet merges
 * cells G12:H12, I12:J12 and C13:D13, E13:F13, G13:H13, I13:J13, i.e. each
 * component spans two adjacent byte columns; sheet "Data Types" lists
 * GPS_POSITION as 96 bit and 96 = 6 x 16 bit, which is consistent. There are
 * therefore NO free bytes in rows 7-8 - the only reserved bytes in the whole
 * beacon are row 1 byte 8 and rows 2-4 (24 bytes).
 *
 * Composite fields (bit tables at the bottom of the HB sheet):
 *
 *   STATUS   (byte 0): BAT_STATUS(1) | LINES_FAULT(1) | AOCS_STATUS(2) | OBC_STATUS(4)
 *   PAYLOADS (byte 6): CLOUD_N_IMP(4) | CRY_STATUS(2) | HEATER_BAT(1) | HEATER_CRY(1)
 *
 * Bit order: the sheet numbers the bits "Bit 1".."Bit 8" with BAT_STATUS on
 * Bit 1 and OBC_STATUS on Bits 5-8. The sheet does NOT say whether "Bit 1" is
 * the least- or most-significant bit. This module takes the conventional
 * reading - Bit 1 = bit 0 (LSB) - and the shift constants below make that
 * choice explicit and single-pointed. THIS IS AN ASSUMPTION TO CONFIRM; if the
 * two bytes come down swapped, only the *_SHIFT constants change.
 *
 * Byte order of the multi-byte fields (GPS_TIMESTAMP, GPS_X..GPS_Z_DOT): the
 * document does not state an endianness. This module writes little-endian
 * (the STM32 native order) using explicit byte assembly, so the on-wire bytes
 * are defined and host-testable rather than inherited from a memcpy(). Also an
 * assumption to confirm.
 *
 * Pass-through fields (the document is silent, so nothing is invented):
 *   - GPS_TIMESTAMP format is literally "Da valutare (32 bit usando formato
 *     UNIX)". This module places the raw 32-bit value the state carries and
 *     performs NO epoch/scaling conversion. The caller owns the format.
 *   - The analogue fields (V_BAT, I_BAT_CHG/DSG, SC_CHG, BAT_SOC, the
 *     temperatures) are declared only as "Digital / 8 bit". No scaling or
 *     offset is specified, so the raw 8-bit values are copied verbatim.
 *
 * Refs: SW_DATA_TYPES.xlsx sheets "HB" and "Data Types"; ECSS-E-ST-40C 5.4
 *       (interface integrity); JPL-182 Rule 14 (check every numeric range).
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * Geometry
 * ------------------------------------------------------------------------- */

/* Bytes per row and number of rows in the HB table. */
#define BEACON_HB_ROW_BYTES   8u
#define BEACON_HB_ROWS        8u

/* Total serialized beacon length: 8 x 8 = 64 bytes. Asserted below. */
#define BEACON_HB_LEN         (BEACON_HB_ROW_BYTES * BEACON_HB_ROWS)

/* Byte offset at which each row starts. Rows 2-4 are reserved. */
#define BEACON_ROW1_OFF       0u
#define BEACON_ROW2_OFF       8u
#define BEACON_ROW3_OFF      16u
#define BEACON_ROW4_OFF      24u
#define BEACON_ROW5_OFF      32u
#define BEACON_ROW6_OFF      40u
#define BEACON_ROW7_OFF      48u
#define BEACON_ROW8_OFF      56u

/* Reserved regions the sheet leaves blank. They are emitted as zero bytes so
 * the frame is deterministic; ground can rely on them until a later revision
 * assigns a meaning (the document has no field there today). */
#define BEACON_OFF_ROW1_RESERVED   7u     /* row 1, byte 8 (column J6 empty)  */
#define BEACON_OFF_RESERVED_ROWS   8u     /* rows 2-4 start here              */
#define BEACON_LEN_RESERVED_ROWS  24u     /* three blank rows x 8 bytes       */

/* ---------------------------------------------------------------------------
 * Field offsets (byte index into the serialized frame)
 * ------------------------------------------------------------------------- */
#define BEACON_OFF_STATUS        (BEACON_ROW1_OFF + 0u)   /*   0 */
#define BEACON_OFF_I_BAT_CHG     (BEACON_ROW1_OFF + 1u)   /*   1 */
#define BEACON_OFF_I_BAT_DSG     (BEACON_ROW1_OFF + 2u)   /*   2 */
#define BEACON_OFF_SC_CHG        (BEACON_ROW1_OFF + 3u)   /*   3 */
#define BEACON_OFF_V_BAT         (BEACON_ROW1_OFF + 4u)   /*   4 */
#define BEACON_OFF_BAT_SOC       (BEACON_ROW1_OFF + 5u)   /*   5 */
#define BEACON_OFF_PAYLOADS      (BEACON_ROW1_OFF + 6u)   /*   6 */

#define BEACON_OFF_GPS_TIMESTAMP (BEACON_ROW5_OFF + 0u)   /*  32 */

#define BEACON_OFF_TEMP_1        (BEACON_ROW6_OFF + 0u)   /*  40 */
#define BEACON_OFF_TEMP_2        (BEACON_ROW6_OFF + 1u)   /*  41 */
#define BEACON_OFF_TEMP_3        (BEACON_ROW6_OFF + 2u)   /*  42 */
#define BEACON_OFF_TEMP_4        (BEACON_ROW6_OFF + 3u)   /*  43 */
#define BEACON_OFF_TEMP_EXT_1    (BEACON_ROW6_OFF + 4u)   /*  44 */
#define BEACON_OFF_TEMP_EXT_2    (BEACON_ROW6_OFF + 5u)   /*  45 */
#define BEACON_OFF_TEMP_EXT_3    (BEACON_ROW6_OFF + 6u)   /*  46 */
#define BEACON_OFF_TEMP_EXT_4    (BEACON_ROW6_OFF + 7u)   /*  47 */

#define BEACON_OFF_TEMP_EXT_5    (BEACON_ROW7_OFF + 0u)   /*  48 */
#define BEACON_OFF_TEMP_EXT_6    (BEACON_ROW7_OFF + 1u)   /*  49 */
#define BEACON_OFF_BAT_TEMP      (BEACON_ROW7_OFF + 2u)   /*  50 */
#define BEACON_OFF_CRY_TEMP      (BEACON_ROW7_OFF + 3u)   /*  51 */
#define BEACON_OFF_GPS_X         (BEACON_ROW7_OFF + 4u)   /*  52, 2 bytes */
#define BEACON_OFF_GPS_Y         (BEACON_ROW7_OFF + 6u)   /*  54, 2 bytes */

#define BEACON_OFF_GPS_Z         (BEACON_ROW8_OFF + 0u)   /*  56, 2 bytes */
#define BEACON_OFF_GPS_X_DOT     (BEACON_ROW8_OFF + 2u)   /*  58, 2 bytes */
#define BEACON_OFF_GPS_Y_DOT     (BEACON_ROW8_OFF + 4u)   /*  60, 2 bytes */
#define BEACON_OFF_GPS_Z_DOT     (BEACON_ROW8_OFF + 6u)   /*  62, 2 bytes */

/* ---------------------------------------------------------------------------
 * Field sizes (bytes)
 * ------------------------------------------------------------------------- */
#define BEACON_SIZE_BYTE         1u
#define BEACON_SIZE_GPS_TIMESTAMP 4u   /* "Data Types": GPS_TIMESTAMP 32 bit */
#define BEACON_SIZE_GPS_COMPONENT 2u   /* GPS_POSITION 96 bit = 6 x 16 bit   */

/* ---------------------------------------------------------------------------
 * STATUS bitfield shifts (byte 0)
 *
 * Sheet HB, bit table row "STATUS": Bit 1 BAT_STATUS, Bit 2 LINES_FAULT,
 * Bits 3-4 AOCS_STATUS, Bits 5-8 OBC_STATUS. Bit 1 is taken as the LSB (see
 * the header note above). Widths from sheet "Data Types" ("Size requirement").
 * ------------------------------------------------------------------------- */
#define BEACON_STATUS_BAT_STATUS_SHIFT   0u   /* 1 bit  */
#define BEACON_STATUS_LINES_FAULT_SHIFT  1u   /* 1 bit  */
#define BEACON_STATUS_AOCS_STATUS_SHIFT  2u   /* 2 bits */
#define BEACON_STATUS_OBC_STATUS_SHIFT   4u   /* 4 bits */

#define BEACON_STATUS_BAT_STATUS_MASK    0x01u
#define BEACON_STATUS_LINES_FAULT_MASK   0x01u
#define BEACON_STATUS_AOCS_STATUS_MASK   0x03u
#define BEACON_STATUS_OBC_STATUS_MASK    0x0Fu

/* ---------------------------------------------------------------------------
 * PAYLOADS bitfield shifts (byte 6)
 *
 * Sheet HB, bit table row "PAYLOADS": Bits 1-4 CLOUD_N_IMP, Bits 5-6
 * CRY_STATUS, Bit 7 HEATER_BAT, Bit 8 HEATER_CRY.
 * ------------------------------------------------------------------------- */
#define BEACON_PAYLOADS_CLOUD_N_IMP_SHIFT 0u   /* 4 bits */
#define BEACON_PAYLOADS_CRY_STATUS_SHIFT  4u   /* 2 bits */
#define BEACON_PAYLOADS_HEATER_BAT_SHIFT  6u   /* 1 bit  */
#define BEACON_PAYLOADS_HEATER_CRY_SHIFT  7u   /* 1 bit  */

#define BEACON_PAYLOADS_CLOUD_N_IMP_MASK  0x0Fu
#define BEACON_PAYLOADS_CRY_STATUS_MASK   0x03u
#define BEACON_PAYLOADS_HEATER_BAT_MASK   0x01u
#define BEACON_PAYLOADS_HEATER_CRY_MASK   0x01u

/* ---------------------------------------------------------------------------
 * AOCS_STATUS - 4 states
 *
 * Sheet "Data Types" row AOCS_STATUS: size available/required 2 bit, notes
 * "OFF, DET, POINTING, FAULT". The 2-bit field has four values (this is the
 * correction to the earlier two-state SPF model, TROVATO.md §1).
 * ------------------------------------------------------------------------- */
typedef enum {
    BEACON_AOCS_OFF      = 0,
    BEACON_AOCS_DET      = 1,
    BEACON_AOCS_POINTING = 2,
    BEACON_AOCS_FAULT    = 3,
} beacon_aocs_status_t;

/* ---------------------------------------------------------------------------
 * Input state
 *
 * One field per named beacon field. STATUS and PAYLOADS are held decomposed
 * (semantic fields, not the packed byte) because that is the state the OBSW
 * actually maintains; beacon_build_hb() packs them with the helpers below.
 * ------------------------------------------------------------------------- */
typedef struct {
    /* Row 1 */
    uint8_t  bat_status;          /* 1 bit  - battery status flag             */
    uint8_t  lines_fault;         /* 1 bit  - power lines fault               */
    beacon_aocs_status_t aocs_status; /* 2 bit - OFF/DET/POINTING/FAULT       */
    uint8_t  obc_status;          /* 4 bit  - OBC status                       */
    uint8_t  i_bat_chg;           /* 8 bit  - battery charge current           */
    uint8_t  i_bat_dsg;           /* 8 bit  - battery discharge current        */
    uint8_t  sc_chg;              /* 8 bit  - solar charger / charge state     */
    uint8_t  v_bat;               /* 8 bit  - battery voltage                  */
    uint8_t  bat_soc;             /* 8 bit  - state of charge                  */
    uint8_t  cloud_n_imp;         /* 4 bit  - CLOUD imaging count/slot         */
    uint8_t  cry_status;          /* 2 bit  - crystal payload status           */
    uint8_t  heater_bat;          /* 1 bit  - battery heater                   */
    uint8_t  heater_cry;          /* 1 bit  - crystal heater                   */

    /* Row 5 - pass-through; format is "Da valutare" (see header note). */
    uint32_t gps_timestamp;

    /* Row 6 */
    uint8_t  temp_1;
    uint8_t  temp_2;
    uint8_t  temp_3;
    uint8_t  temp_4;
    uint8_t  temp_ext_1;
    uint8_t  temp_ext_2;
    uint8_t  temp_ext_3;
    uint8_t  temp_ext_4;

    /* Row 7 */
    uint8_t  temp_ext_5;
    uint8_t  temp_ext_6;
    uint8_t  bat_temp;
    uint8_t  cry_temp;
    uint16_t gps_x;
    uint16_t gps_y;

    /* Row 8 */
    uint16_t gps_z;
    uint16_t gps_x_dot;
    uint16_t gps_y_dot;
    uint16_t gps_z_dot;
} beacon_hb_state_t;

/* ---------------------------------------------------------------------------
 * Bitfield helpers
 *
 * STATUS and PAYLOADS are simply packed bytes; these are the single place
 * where the bit assignment lives, so a caller that only has the raw byte
 * (e.g. a consumer of a received frame) can take it apart with the inverse.
 * ------------------------------------------------------------------------- */

/* Pack the four STATUS sub-fields into byte 0 of the beacon. Each argument is
 * masked to its documented width, so an over-wide value cannot bleed into a
 * neighbouring field. */
uint8_t beacon_status_pack(uint8_t bat_status,
                           uint8_t lines_fault,
                           beacon_aocs_status_t aocs_status,
                           uint8_t obc_status);

/* Inverse of beacon_status_pack(). Any output pointer may be NULL if that
 * field is not wanted. aocs_status is returned as the enum, never as a raw
 * 0-3, so a value outside the four is impossible by construction. */
void beacon_status_unpack(uint8_t status,
                          uint8_t *bat_status,
                          uint8_t *lines_fault,
                          beacon_aocs_status_t *aocs_status,
                          uint8_t *obc_status);

/* Pack the four PAYLOADS sub-fields into byte 6 of the beacon. */
uint8_t beacon_payloads_pack(uint8_t cloud_n_imp,
                             uint8_t cry_status,
                             uint8_t heater_bat,
                             uint8_t heater_cry);

/* Inverse of beacon_payloads_pack(). Any output pointer may be NULL. */
void beacon_payloads_unpack(uint8_t payloads,
                            uint8_t *cloud_n_imp,
                            uint8_t *cry_status,
                            uint8_t *heater_bat,
                            uint8_t *heater_cry);

/* ---------------------------------------------------------------------------
 * Serialization
 * ------------------------------------------------------------------------- */

/* Serialize `st` into `out` according to the layout above.
 *
 * `out_len` is the capacity of `out`. The reserved bytes (row 1 byte 8 and
 * rows 2-4) are written as zero so the frame is fully deterministic.
 *
 * Returns BEACON_HB_LEN on success, 0 on a NULL pointer or a buffer shorter
 * than BEACON_HB_LEN. 0 is never a valid length, so a caller that ignores the
 * return value cannot mistake a short frame for a serialized one. */
size_t beacon_build_hb(const beacon_hb_state_t *st, uint8_t *out, size_t out_len);

/* ---- Compile-time layout checks (the constants above must describe a frame
 * that exactly tiles 64 bytes, with no field straddling a row boundary by
 * accident). ---- */
_Static_assert(BEACON_HB_LEN == 64u, "HB beacon must be 8 rows x 8 bytes");

_Static_assert(BEACON_OFF_STATUS + BEACON_SIZE_BYTE <= BEACON_HB_LEN &&
               BEACON_OFF_PAYLOADS + BEACON_SIZE_BYTE <= BEACON_HB_LEN,
               "row 1 fields out of range");
_Static_assert(BEACON_OFF_GPS_TIMESTAMP + BEACON_SIZE_GPS_TIMESTAMP <= BEACON_HB_LEN,
               "GPS_TIMESTAMP out of range");
_Static_assert(BEACON_OFF_GPS_X + BEACON_SIZE_GPS_COMPONENT <= BEACON_HB_LEN &&
               BEACON_OFF_GPS_Z_DOT + BEACON_SIZE_GPS_COMPONENT <= BEACON_HB_LEN,
               "GPS components out of range");

/* The last GPS component must end exactly on the last byte of the frame. */
_Static_assert(BEACON_OFF_GPS_Z_DOT + BEACON_SIZE_GPS_COMPONENT == BEACON_HB_LEN,
               "row 8 must end at the end of the frame");

#endif /* BEACON_H */
