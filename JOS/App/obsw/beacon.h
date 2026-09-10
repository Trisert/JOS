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
 *   Row 5  GPS_TIMESTAMP (32 bit, spans bytes 1-4 - merged cell C10:F10) | (rsv, bytes 5-8)
 *   Row 6  TEMP_1 | TEMP_2 | TEMP_3 | TEMP_4 | TEMP_EXT_1 | TEMP_EXT_2 | TEMP_EXT_3 | TEMP_EXT_4
 *   Row 7  TEMP_EXT_5 | TEMP_EXT_6 | BAT_TEMP | CRY_TEMP | GPS_X (2B) | GPS_Y (2B)
 *   Row 8  GPS_Z (2B) | GPS_X_DOT (2B) | GPS_Y_DOT (2B) | GPS_Z_DOT (2B)
 *
 * IMPORTANT (width correction vs the original task brief): the six GPS
 * components in rows 7-8 are EACH TWO BYTES, not one. The HB sheet merges
 * cells G12:H12, I12:J12 and C13:D13, E13:F13, G13:H13, I13:J13, i.e. each
 * component spans two adjacent byte columns; sheet "Data Types" lists
 * GPS_POSITION as 96 bit and 96 = 6 x 16 bit, which is consistent. There are
 * therefore NO free bytes in rows 7-8.
 *
 * Reserved bytes: the sheet leaves a NAMED-field-free gap in three places,
 * 29 bytes in total. 35 named bytes + 29 reserved bytes = 64 (an earlier
 * revision of this comment accounted for only 25 reserved bytes - row 1 byte
 * 8 plus rows 2-4 - and left the 4-byte gap at offsets 36..39 unaccounted):
 *
 *   row 1 byte 8   (offset  7)       1 byte   BEACON_OFF_ROW1_RESERVED
 *   rows 2-4       (offsets 8..31)  24 bytes  BEACON_OFF_RESERVED_ROWS
 *   row 5 bytes 5-8(offsets 36..39)  4 bytes  BEACON_OFF_ROW5_RESERVED
 *
 * Row 5 is the region that was previously missed: GPS_TIMESTAMP fills bytes
 * 1-4 (merged C10:F10) and G10:J10 is blank. All three regions are emitted as
 * 0x00 by beacon_build_hb().
 *
 * Composite fields (bit tables at the bottom of the HB sheet):
 *
 *   STATUS   (byte 0): BAT_STATUS(1) | LINES_FAULT(1) | AOCS_STATUS(2) | OBC_STATUS(4)
 *   PAYLOADS (byte 6): CLOUD_N_IMP(4) | CRY_STATUS(2) | HEATER_BAT(1) | HEATER_CRY(1)
 *
 * Bit order: the sheet numbers the bits "Bit 1".."Bit 8" with BAT_STATUS on
 * Bit 1 and OBC_STATUS on Bits 5-8. "Bit 1" is the MOST significant bit of the
 * byte (MSB-first), so the *_SHIFT constants below place BAT_STATUS on bit 7
 * and OBC_STATUS on bits 3..0. Evidence (not an assumption): the sibling doc
 * TTC packets.xlsx, sheet "Task details", records the same 8-bit field twice -
 * columns "TEC bin ID" and "TEC hex ID" - and the pairs agree only under an
 * MSB-first reading (task ID 1 -> bin 00000001 = hex 0x01; task ID 2 ->
 * bin 00000010 = 0x02, and so on up to ID 63 -> 00111111 = 0x3F). The
 * leftmost documented bit is therefore the MSB, and a documented "Bit N" of an
 * 8-bit row maps to hardware bit (8 - N). The *_SHIFT constants are the single
 * point where that mapping lives.
 *
 * =========================================================================
 * BLOCKING OPEN ITEM - byte order of the multi-byte fields
 * (GPS_TIMESTAMP, GPS_X/GPS_Y/GPS_Z and their *_DOT rates)
 * =========================================================================
 * The HB sheet states no endianness. The whole document set shipped under
 * /root/sp_findings and /tmp/spdl was searched for an explicit statement and
 * NONE exists (the only "endianness" hit is SPI_FIRSTBIT_MSB in the scratch
 * SW_MAIN.c, which is a peripheral setup and unrelated to the beacon). This
 * module therefore keeps little-endian (the STM32 native order), written with
 * explicit byte assembly so the on-wire bytes are defined and host-testable
 * rather than inherited from a memcpy().
 *
 * THIS IS UNCONFIRMED AND MUST BE RESOLVED BEFORE THE FRAME IS FROZEN. It is
 * in direct tension with JOS/App/comms/comms_validate.h:24, which declares the
 * authenticated frame on the SAME link "big-endian on the wire". If that
 * convention also governs the beacon, every multi-byte field here is
 * byte-swapped. The change would be localised to beacon_put_u16_le() /
 * beacon_put_u32_le() in beacon.c once ground states the rule.
 * =========================================================================
 *
 * Pass-through fields (the document is silent, so nothing is invented):
 *   - GPS_TIMESTAMP format is literally "Da valutare (32 bit usando formato
 *     UNIX)". This module places the raw 32-bit value the state carries and
 *     performs NO epoch/scaling conversion. The caller owns the format.
 *   - The analogue fields (V_BAT, I_BAT_CHG/DSG, SC_CHG, BAT_SOC, the
 *     temperatures) are declared only as "Digital / 8 bit". No scaling or
 *     offset is specified, so the raw 8-bit values are copied verbatim.
 *
 * Refs: SW_DATA_TYPES.xlsx sheets "HB" and "Data Types"; TTC packets.xlsx
 *       sheet "Task details" (bit order); ECSS-E-ST-40C 5.4 (interface
 *       integrity); JPL-182 Rule 14 (check every numeric range).
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
#define BEACON_OFF_ROW5_RESERVED  (BEACON_ROW5_OFF + 4u) /* row 5, bytes 5-8 (G10:J10 empty) */
#define BEACON_LEN_ROW5_RESERVED   4u     /* four blank bytes at the end of row 5 */

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
/* Row 1 byte 8 is reserved: BEACON_OFF_ROW1_RESERVED (7). */

#define BEACON_OFF_GPS_TIMESTAMP (BEACON_ROW5_OFF + 0u)   /*  32, 4 bytes */

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
 * Bits 3-4 AOCS_STATUS, Bits 5-8 OBC_STATUS. "Bit 1" is the MSB (see the
 * header note), so Bit N of this 8-bit byte maps to hardware bit (8 - N):
 * BAT_STATUS bit 7, LINES_FAULT bit 6, AOCS_STATUS bits 5..4, OBC_STATUS
 * bits 3..0. Widths from sheet "Data Types" ("Size requirement").
 * ------------------------------------------------------------------------- */
#define BEACON_STATUS_BAT_STATUS_SHIFT   7u   /* 1 bit  */
#define BEACON_STATUS_LINES_FAULT_SHIFT  6u   /* 1 bit  */
#define BEACON_STATUS_AOCS_STATUS_SHIFT  4u   /* 2 bits */
#define BEACON_STATUS_OBC_STATUS_SHIFT   0u   /* 4 bits */

#define BEACON_STATUS_BAT_STATUS_MASK    0x01u
#define BEACON_STATUS_LINES_FAULT_MASK   0x01u
#define BEACON_STATUS_AOCS_STATUS_MASK   0x03u
#define BEACON_STATUS_OBC_STATUS_MASK    0x0Fu

/* ---------------------------------------------------------------------------
 * PAYLOADS bitfield shifts (byte 6)
 *
 * Sheet HB, bit table row "PAYLOADS": Bits 1-4 CLOUD_N_IMP, Bits 5-6
 * CRY_STATUS, Bit 7 HEATER_BAT, Bit 8 HEATER_CRY. Under the same MSB-first
 * mapping: CLOUD_N_IMP bits 7..4, CRY_STATUS bits 3..2, HEATER_BAT bit 1,
 * HEATER_CRY bit 0.
 * ------------------------------------------------------------------------- */
#define BEACON_PAYLOADS_CLOUD_N_IMP_SHIFT 4u   /* 4 bits */
#define BEACON_PAYLOADS_CRY_STATUS_SHIFT  2u   /* 2 bits */
#define BEACON_PAYLOADS_HEATER_BAT_SHIFT  1u   /* 1 bit  */
#define BEACON_PAYLOADS_HEATER_CRY_SHIFT  0u   /* 1 bit  */

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
 * `out_len` is the capacity of `out`. The reserved bytes (row 1 byte 8,
 * rows 2-4, row 5 bytes 5-8) are written as zero so the frame is fully
 * deterministic.
 *
 * Returns BEACON_HB_LEN on success, 0 on a NULL pointer or a buffer shorter
 * than BEACON_HB_LEN. 0 is never a valid length, so a caller that ignores the
 * return value cannot mistake a short frame for a serialized one. */
size_t beacon_build_hb(const beacon_hb_state_t *st, uint8_t *out, size_t out_len);

/* ---- Compile-time layout checks.
 *
 * These deliberately compare the BEACON_* constants against LITERAL byte
 * indices taken from the HB sheet (and against each other where contiguity is
 * the invariant) - NOT against BEACON_HB_LEN or against one another in a way
 * that would let a wrong constant satisfy its own check. A mutation such as
 * moving BEACON_OFF_GPS_X from +4 to +5, or swapping a *_SHIFT constant back
 * to the old LSB-first value, now fails the build instead of compiling
 * silently. ---------------------------------------------------------------- */

_Static_assert(BEACON_HB_LEN == 64u, "HB beacon must be 8 rows x 8 bytes");
_Static_assert(BEACON_HB_ROW_BYTES == 8u && BEACON_HB_ROWS == 8u,
               "HB table must be 8 rows x 8 byte columns");

/* Row starts, against the literal byte indices of the sheet. */
_Static_assert(BEACON_ROW1_OFF == 0u,  "row 1 must start at byte 0");
_Static_assert(BEACON_ROW2_OFF == 8u,  "row 2 must start at byte 8");
_Static_assert(BEACON_ROW3_OFF == 16u, "row 3 must start at byte 16");
_Static_assert(BEACON_ROW4_OFF == 24u, "row 4 must start at byte 24");
_Static_assert(BEACON_ROW5_OFF == 32u, "row 5 must start at byte 32");
_Static_assert(BEACON_ROW6_OFF == 40u, "row 6 must start at byte 40");
_Static_assert(BEACON_ROW7_OFF == 48u, "row 7 must start at byte 48");
_Static_assert(BEACON_ROW8_OFF == 56u, "row 8 must start at byte 56");

/* Row 1 fields, against the literal byte columns. */
_Static_assert(BEACON_OFF_STATUS == 0u,   "STATUS is byte 1 (offset 0)");
_Static_assert(BEACON_OFF_I_BAT_CHG == 1u && BEACON_OFF_I_BAT_DSG == 2u &&
               BEACON_OFF_SC_CHG == 3u && BEACON_OFF_V_BAT == 4u &&
               BEACON_OFF_BAT_SOC == 5u && BEACON_OFF_PAYLOADS == 6u,
               "row 1 analogue fields must occupy bytes 2..7 (offsets 1..6)");

/* Reserved regions: row 1 byte 8, rows 2-4, and row 5 bytes 5-8. */
_Static_assert(BEACON_OFF_ROW1_RESERVED == 7u, "row 1 byte 8 is reserved at offset 7");
_Static_assert(BEACON_OFF_RESERVED_ROWS == 8u && BEACON_LEN_RESERVED_ROWS == 24u,
               "rows 2-4 must be the 24 bytes at offsets 8..31");
_Static_assert(BEACON_OFF_ROW5_RESERVED == 36u, "row 5 reserved bytes must start at offset 36");
_Static_assert(BEACON_LEN_ROW5_RESERVED == 4u,  "row 5 must have exactly 4 reserved bytes");
_Static_assert(BEACON_OFF_ROW5_RESERVED + BEACON_LEN_ROW5_RESERVED == BEACON_ROW6_OFF,
               "row 5 reserved region must end exactly where row 6 begins");

/* Row 5: GPS_TIMESTAMP (4 bytes) then the reserved gap. */
_Static_assert(BEACON_OFF_GPS_TIMESTAMP == 32u, "GPS_TIMESTAMP must start at offset 32");
_Static_assert(BEACON_SIZE_GPS_TIMESTAMP == 4u, "GPS_TIMESTAMP must be 4 bytes");
_Static_assert(BEACON_OFF_GPS_TIMESTAMP + BEACON_SIZE_GPS_TIMESTAMP == BEACON_OFF_ROW5_RESERVED,
               "GPS_TIMESTAMP must end exactly where the row 5 reserved gap begins");

/* Row 6: eight contiguous 1-byte fields. */
_Static_assert(BEACON_OFF_TEMP_1 == 40u, "TEMP_1 must start at offset 40");
_Static_assert(BEACON_OFF_TEMP_EXT_4 == BEACON_OFF_TEMP_1 + 7u,
               "row 6 must be eight contiguous bytes");

/* Row 7: four 1-byte fields, then two contiguous 2-byte GPS components. */
_Static_assert(BEACON_OFF_TEMP_EXT_5 == 48u, "TEMP_EXT_5 must start at offset 48");
_Static_assert(BEACON_OFF_GPS_X == 52u, "GPS_X must start at offset 52");
_Static_assert(BEACON_OFF_GPS_Y == BEACON_OFF_GPS_X + BEACON_SIZE_GPS_COMPONENT,
               "GPS_Y must follow GPS_X with no gap");
_Static_assert(BEACON_OFF_GPS_Y + BEACON_SIZE_GPS_COMPONENT == BEACON_ROW8_OFF,
               "row 7 must end exactly where row 8 begins");

/* Row 8: four contiguous 2-byte GPS components, ending on the frame end. */
_Static_assert(BEACON_OFF_GPS_Z == 56u, "GPS_Z must start at offset 56");
_Static_assert(BEACON_OFF_GPS_X_DOT == BEACON_OFF_GPS_Z + BEACON_SIZE_GPS_COMPONENT,
               "GPS_X_DOT must follow GPS_Z with no gap");
_Static_assert(BEACON_OFF_GPS_Y_DOT == BEACON_OFF_GPS_X_DOT + BEACON_SIZE_GPS_COMPONENT,
               "GPS_Y_DOT must follow GPS_X_DOT with no gap");
_Static_assert(BEACON_OFF_GPS_Z_DOT == BEACON_OFF_GPS_Y_DOT + BEACON_SIZE_GPS_COMPONENT,
               "GPS_Z_DOT must follow GPS_Y_DOT with no gap");
_Static_assert(BEACON_OFF_GPS_Z_DOT + BEACON_SIZE_GPS_COMPONENT == BEACON_HB_LEN,
               "row 8 must end exactly at the end of the frame");

/* Bit assignment: MSB-first, and the sub-field widths from sheet "Data Types". */
_Static_assert(BEACON_STATUS_BAT_STATUS_SHIFT == 7u &&
               BEACON_STATUS_LINES_FAULT_SHIFT == 6u &&
               BEACON_STATUS_AOCS_STATUS_SHIFT == 4u &&
               BEACON_STATUS_OBC_STATUS_SHIFT == 0u,
               "STATUS bit order must be MSB-first (Bit 1 = bit 7)");
_Static_assert(BEACON_PAYLOADS_CLOUD_N_IMP_SHIFT == 4u &&
               BEACON_PAYLOADS_CRY_STATUS_SHIFT == 2u &&
               BEACON_PAYLOADS_HEATER_BAT_SHIFT == 1u &&
               BEACON_PAYLOADS_HEATER_CRY_SHIFT == 0u,
               "PAYLOADS bit order must be MSB-first (Bit 1 = bit 7)");
_Static_assert(BEACON_STATUS_BAT_STATUS_MASK == 0x01u &&
               BEACON_STATUS_AOCS_STATUS_MASK == 0x03u &&
               BEACON_STATUS_OBC_STATUS_MASK == 0x0Fu &&
               BEACON_PAYLOADS_CLOUD_N_IMP_MASK == 0x0Fu &&
               BEACON_PAYLOADS_CRY_STATUS_MASK == 0x03u,
               "sub-field masks must match the documented field widths");

#endif /* BEACON_H */
