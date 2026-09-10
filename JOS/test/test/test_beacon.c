/* ---------------------------------------------------------------------------
 * test_beacon.c - unit tests for App/obsw/beacon.c
 *
 * What is pinned here, and why:
 *
 *   1. The byte OFFSET of every field, asserted against LITERAL indices taken
 *      from sheet "HB" of SW_DATA_TYPES.xlsx (row 1 = bytes 0..7, row 5 at
 *      byte 32, row 6 at byte 40, row 7 at byte 48, row 8 at byte 56). The
 *      assertions use raw numbers on purpose - NOT the BEACON_OFF_* constants
 *      - so that a mistake in a constant and a mistake in the code cannot
 *      cancel out and pass. If someone renumbers a constant, these tests stay
 *      pinned to the document and fail.
 *
 *   2. The frame length: exactly 64 bytes (8 rows x 8 bytes).
 *
 *   3. The GPS components are 2 bytes each (the merged-cell width correction:
 *      GPS_POSITION = 96 bit = 6 x 16 bit), checked at their literal offsets.
 *
 *   4. Round trips of the two bitfields, the bit position of every sub-field
 *      (raw bit values, again literal), and all four AOCS_STATUS values.
 *
 *   5. The reserved bytes (row 1 byte 8 and rows 2-4) come out zero.
 *
 * Refs: ECSS-E-ST-40C 5.5 (validation), NASA-STD-8739.8 (verification
 *       evidence), JPL-182 Rule 31.
 * ------------------------------------------------------------------------- */
#include "unity.h"
#include "beacon.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

void setUp(void)
{
}

void tearDown(void)
{
}

/* A state with a distinct, non-repeating value in every field, so a swap
 * between two fields shows up as two failed assertions rather than one. */
static void fill_distinct(beacon_hb_state_t *st)
{
    st->bat_status   = 1u;
    st->lines_fault  = 1u;
    st->aocs_status  = BEACON_AOCS_POINTING; /* = 2 -> bits 2..3 = 0b10 */
    st->obc_status   = 0xAu;                  /* bits 4..7 = 0b1010      */
    st->i_bat_chg    = 0x11u;
    st->i_bat_dsg    = 0x22u;
    st->sc_chg       = 0x33u;
    st->v_bat        = 0x44u;
    st->bat_soc      = 0x55u;
    st->cloud_n_imp  = 0x9u;                  /* bits 0..3                */
    st->cry_status   = 0x2u;                  /* bits 4..5                */
    st->heater_bat   = 1u;                    /* bit 6                    */
    st->heater_cry   = 0u;                    /* bit 7                    */

    st->gps_timestamp = 0xAABBCCDDu;

    st->temp_1     = 0x66u;
    st->temp_2     = 0x77u;
    st->temp_3     = 0x88u;
    st->temp_4     = 0x99u;
    st->temp_ext_1 = 0xAAu;
    st->temp_ext_2 = 0xBBu;
    st->temp_ext_3 = 0xCCu;
    st->temp_ext_4 = 0xDDu;

    st->temp_ext_5 = 0xEEu;
    st->temp_ext_6 = 0xFFu;
    st->bat_temp   = 0x12u;
    st->cry_temp   = 0x34u;
    st->gps_x      = 0x1234u;
    st->gps_y      = 0x5678u;

    st->gps_z      = 0x9ABCu;
    st->gps_x_dot  = 0xDEF0u;
    st->gps_y_dot  = 0x1357u;
    st->gps_z_dot  = 0x2468u;
}

/* =====================================================================
 * 1. Frame length
 * ===================================================================== */

/* The doc table is 8 rows of 8 bytes; the whole beacon is 64 bytes. */
void test_beacon_length_macro_is_64(void)
{
    TEST_ASSERT_EQUAL_UINT(64u, (unsigned)BEACON_HB_LEN);
    TEST_ASSERT_EQUAL_UINT(8u, (unsigned)BEACON_HB_ROW_BYTES);
    TEST_ASSERT_EQUAL_UINT(8u, (unsigned)BEACON_HB_ROWS);
}

/* The serializer reports, and writes, exactly 64 bytes. */
void test_beacon_build_writes_exactly_64_bytes(void)
{
    beacon_hb_state_t st;
    uint8_t out[64];
    size_t n;

    fill_distinct(&st);
    n = beacon_build_hb(&st, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(64u, (unsigned)n);
}

/* A buffer one byte short must be rejected, not filled partially: a caller
 * that ignored the length would otherwise send a truncated frame. */
void test_beacon_build_rejects_short_buffer(void)
{
    beacon_hb_state_t st;
    uint8_t out[64];

    fill_distinct(&st);
    TEST_ASSERT_EQUAL_UINT(0u, (unsigned)beacon_build_hb(&st, out, 63u));
}

void test_beacon_build_rejects_null_arguments(void)
{
    beacon_hb_state_t st;
    uint8_t out[64];

    fill_distinct(&st);
    TEST_ASSERT_EQUAL_UINT(0u, (unsigned)beacon_build_hb(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0u, (unsigned)beacon_build_hb(&st, NULL, sizeof(out)));
}

/* =====================================================================
 * 2. Field offsets - row 1 (literal indices from the HB sheet)
 * ===================================================================== */

void test_row1_fields_at_literal_offsets(void)
{
    beacon_hb_state_t st;
    uint8_t out[64];

    fill_distinct(&st);
    TEST_ASSERT_EQUAL_UINT(64u, (unsigned)beacon_build_hb(&st, out, sizeof(out)));

    /* Byte 1 = STATUS. bat_status=1 (bit0), lines_fault=1 (bit1),
     * AOCS POINTING=2 (bits2..3), obc_status=0xA (bits4..7)
     *  -> 0b1010_1011 = 0xAB. */
    TEST_ASSERT_EQUAL_HEX8(0xABu, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x11u, out[1]); /* I_BAT_CHG  */
    TEST_ASSERT_EQUAL_HEX8(0x22u, out[2]); /* I_BAT_DSG  */
    TEST_ASSERT_EQUAL_HEX8(0x33u, out[3]); /* SC_CHG     */
    TEST_ASSERT_EQUAL_HEX8(0x44u, out[4]); /* V_BAT      */
    TEST_ASSERT_EQUAL_HEX8(0x55u, out[5]); /* BAT_SOC    */

    /* Byte 7 = PAYLOADS. cloud_n_imp=9 (bits0..3), cry_status=2 (bits4..5),
     * heater_bat=1 (bit6), heater_cry=0 (bit7) -> 0b0110_1001 = 0x69. */
    TEST_ASSERT_EQUAL_HEX8(0x69u, out[6]);
}

/* Row 1 byte 8 is blank in the sheet: it must not carry anything. */
void test_row1_byte8_is_reserved_zero(void)
{
    beacon_hb_state_t st;
    uint8_t out[64];

    fill_distinct(&st);
    (void)beacon_build_hb(&st, out, sizeof(out));
    TEST_ASSERT_EQUAL_HEX8(0x00u, out[7]);
}

/* =====================================================================
 * 3. Rows 2-4 reserved (bytes 8..31)
 * ===================================================================== */

void test_rows_2_3_4_are_reserved_zero(void)
{
    beacon_hb_state_t st;
    uint8_t out[64];
    int i;

    fill_distinct(&st);
    (void)beacon_build_hb(&st, out, sizeof(out));

    /* The sheet has no field name in rows 2-4, so those 24 bytes stay 0. */
    for (i = 8; i <= 31; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00u, out[i], "reserved byte not zero");
    }
}

/* =====================================================================
 * 4. Row 5 - GPS_TIMESTAMP (byte 32, 32 bit, little-endian)
 * ===================================================================== */

void test_gps_timestamp_at_literal_offset_32(void)
{
    beacon_hb_state_t st;
    uint8_t out[64];

    fill_distinct(&st);
    (void)beacon_build_hb(&st, out, sizeof(out));

    /* Pass-through: the raw 32-bit value, LSB first. No format conversion. */
    TEST_ASSERT_EQUAL_HEX8(0xDDu, out[32]);
    TEST_ASSERT_EQUAL_HEX8(0xCCu, out[33]);
    TEST_ASSERT_EQUAL_HEX8(0xBBu, out[34]);
    TEST_ASSERT_EQUAL_HEX8(0xAAu, out[35]);
}

/* =====================================================================
 * 5. Row 6 - temperatures (bytes 40..47)
 * ===================================================================== */

void test_row6_temperatures_at_literal_offsets(void)
{
    beacon_hb_state_t st;
    uint8_t out[64];

    fill_distinct(&st);
    (void)beacon_build_hb(&st, out, sizeof(out));

    TEST_ASSERT_EQUAL_HEX8(0x66u, out[40]); /* TEMP_1     */
    TEST_ASSERT_EQUAL_HEX8(0x77u, out[41]); /* TEMP_2     */
    TEST_ASSERT_EQUAL_HEX8(0x88u, out[42]); /* TEMP_3     */
    TEST_ASSERT_EQUAL_HEX8(0x99u, out[43]); /* TEMP_4     */
    TEST_ASSERT_EQUAL_HEX8(0xAAu, out[44]); /* TEMP_EXT_1 */
    TEST_ASSERT_EQUAL_HEX8(0xBBu, out[45]); /* TEMP_EXT_2 */
    TEST_ASSERT_EQUAL_HEX8(0xCCu, out[46]); /* TEMP_EXT_3 */
    TEST_ASSERT_EQUAL_HEX8(0xDDu, out[47]); /* TEMP_EXT_4 */
}

/* =====================================================================
 * 6. Row 7 - temps, battery temp and the first two GPS components
 * ===================================================================== */

void test_row7_fields_at_literal_offsets(void)
{
    beacon_hb_state_t st;
    uint8_t out[64];

    fill_distinct(&st);
    (void)beacon_build_hb(&st, out, sizeof(out));

    TEST_ASSERT_EQUAL_HEX8(0xEEu, out[48]); /* TEMP_EXT_5 */
    TEST_ASSERT_EQUAL_HEX8(0xFFu, out[49]); /* TEMP_EXT_6 */
    TEST_ASSERT_EQUAL_HEX8(0x12u, out[50]); /* BAT_TEMP   */
    TEST_ASSERT_EQUAL_HEX8(0x34u, out[51]); /* CRY_TEMP   */

    /* GPS_X is a 2-byte field (merged cells G12:H12), bytes 52..53, LSB first. */
    TEST_ASSERT_EQUAL_HEX8(0x34u, out[52]);
    TEST_ASSERT_EQUAL_HEX8(0x12u, out[53]);
    /* GPS_Y, bytes 54..55 (merged I12:J12). */
    TEST_ASSERT_EQUAL_HEX8(0x78u, out[54]);
    TEST_ASSERT_EQUAL_HEX8(0x56u, out[55]);
}

/* =====================================================================
 * 7. Row 8 - the four remaining GPS components (bytes 56..63)
 * ===================================================================== */

void test_row8_gps_components_at_literal_offsets(void)
{
    beacon_hb_state_t st;
    uint8_t out[64];

    fill_distinct(&st);
    (void)beacon_build_hb(&st, out, sizeof(out));

    /* GPS_Z, bytes 56..57 (C13:D13). */
    TEST_ASSERT_EQUAL_HEX8(0xBCu, out[56]);
    TEST_ASSERT_EQUAL_HEX8(0x9Au, out[57]);
    /* GPS_X_DOT, bytes 58..59 (E13:F13). */
    TEST_ASSERT_EQUAL_HEX8(0xF0u, out[58]);
    TEST_ASSERT_EQUAL_HEX8(0xDEu, out[59]);
    /* GPS_Y_DOT, bytes 60..61 (G13:H13). */
    TEST_ASSERT_EQUAL_HEX8(0x57u, out[60]);
    TEST_ASSERT_EQUAL_HEX8(0x13u, out[61]);
    /* GPS_Z_DOT, bytes 62..63 (I13:J13). */
    TEST_ASSERT_EQUAL_HEX8(0x68u, out[62]);
    TEST_ASSERT_EQUAL_HEX8(0x24u, out[63]);
}

/* The six GPS components really occupy twelve bytes: prove it by placing a
 * value with a different high byte in each and checking every byte. */
void test_all_six_gps_components_two_bytes_each(void)
{
    beacon_hb_state_t st;
    uint8_t out[64];

    memset(&st, 0, sizeof(st));
    st.aocs_status = BEACON_AOCS_OFF;
    st.gps_x     = 0x0102u;
    st.gps_y     = 0x0304u;
    st.gps_z     = 0x0506u;
    st.gps_x_dot = 0x0708u;
    st.gps_y_dot = 0x090Au;
    st.gps_z_dot = 0x0B0Cu;

    TEST_ASSERT_EQUAL_UINT(64u, (unsigned)beacon_build_hb(&st, out, sizeof(out)));

    TEST_ASSERT_EQUAL_HEX8(0x02u, out[52]); TEST_ASSERT_EQUAL_HEX8(0x01u, out[53]);
    TEST_ASSERT_EQUAL_HEX8(0x04u, out[54]); TEST_ASSERT_EQUAL_HEX8(0x03u, out[55]);
    TEST_ASSERT_EQUAL_HEX8(0x06u, out[56]); TEST_ASSERT_EQUAL_HEX8(0x05u, out[57]);
    TEST_ASSERT_EQUAL_HEX8(0x08u, out[58]); TEST_ASSERT_EQUAL_HEX8(0x07u, out[59]);
    TEST_ASSERT_EQUAL_HEX8(0x0Au, out[60]); TEST_ASSERT_EQUAL_HEX8(0x09u, out[61]);
    TEST_ASSERT_EQUAL_HEX8(0x0Cu, out[62]); TEST_ASSERT_EQUAL_HEX8(0x0Bu, out[63]);
}

/* =====================================================================
 * 8. STATUS bitfield - bit positions and round trip
 * ===================================================================== */

/* Each sub-field sits on the exact bit the HB sheet assigns, checked with raw
 * bit values (not the BEACON_*_SHIFT constants). */
void test_status_bit_positions_are_literal(void)
{
    /* Bit 1 BAT_STATUS -> 0x01 */
    TEST_ASSERT_EQUAL_HEX8(0x01u, beacon_status_pack(1u, 0u, BEACON_AOCS_OFF, 0u));
    /* Bit 2 LINES_FAULT -> 0x02 */
    TEST_ASSERT_EQUAL_HEX8(0x02u, beacon_status_pack(0u, 1u, BEACON_AOCS_OFF, 0u));
    /* Bits 3-4 AOCS_STATUS: DET(1) -> 0x04, POINTING(2) -> 0x08, FAULT(3) -> 0x0C */
    TEST_ASSERT_EQUAL_HEX8(0x04u, beacon_status_pack(0u, 0u, BEACON_AOCS_DET, 0u));
    TEST_ASSERT_EQUAL_HEX8(0x08u, beacon_status_pack(0u, 0u, BEACON_AOCS_POINTING, 0u));
    TEST_ASSERT_EQUAL_HEX8(0x0Cu, beacon_status_pack(0u, 0u, BEACON_AOCS_FAULT, 0u));
    /* Bits 5-8 OBC_STATUS: 0x1 -> 0x10, 0x8 -> 0x80, 0xF -> 0xF0 */
    TEST_ASSERT_EQUAL_HEX8(0x10u, beacon_status_pack(0u, 0u, BEACON_AOCS_OFF, 0x1u));
    TEST_ASSERT_EQUAL_HEX8(0x80u, beacon_status_pack(0u, 0u, BEACON_AOCS_OFF, 0x8u));
    TEST_ASSERT_EQUAL_HEX8(0xF0u, beacon_status_pack(0u, 0u, BEACON_AOCS_OFF, 0xFu));
}

/* Round trip over the full documented range of every sub-field. */
void test_status_round_trip_all_values(void)
{
    unsigned b, l, a, o;

    for (o = 0u; o < 16u; o++) {
        for (a = 0u; a < 4u; a++) {
            for (b = 0u; b < 2u; b++) {
                for (l = 0u; l < 2u; l++) {
                    uint8_t status = beacon_status_pack((uint8_t)b, (uint8_t)l,
                                                        (beacon_aocs_status_t)a,
                                                        (uint8_t)o);
                    uint8_t rb = 0xEEu, rl = 0xEEu, ro = 0xEEu;
                    beacon_aocs_status_t ra = (beacon_aocs_status_t)0xEE;

                    beacon_status_unpack(status, &rb, &rl, &ra, &ro);
                    TEST_ASSERT_EQUAL_UINT(b, (unsigned)rb);
                    TEST_ASSERT_EQUAL_UINT(l, (unsigned)rl);
                    TEST_ASSERT_EQUAL_UINT(a, (unsigned)ra);
                    TEST_ASSERT_EQUAL_UINT(o, (unsigned)ro);
                }
            }
        }
    }
}

/* An over-wide argument must be masked into its own field, never bleed into a
 * neighbour (0xFF bat_status must not light up LINES_FAULT). */
void test_status_pack_masks_to_field_width(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x01u, beacon_status_pack(0xFFu, 0u, BEACON_AOCS_OFF, 0u));
    TEST_ASSERT_EQUAL_HEX8(0x02u, beacon_status_pack(0u, 0xFFu, BEACON_AOCS_OFF, 0u));
    TEST_ASSERT_EQUAL_HEX8(0xF0u, beacon_status_pack(0u, 0u, BEACON_AOCS_OFF, 0xFFu));
}

/* The unpack helper tolerates NULL output pointers per field. */
void test_status_unpack_accepts_null_outputs(void)
{
    uint8_t status = beacon_status_pack(1u, 0u, BEACON_AOCS_FAULT, 0x5u);

    beacon_status_unpack(status, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_HEX8(0x5Du, status); /* unchanged, no crash */
}

/* =====================================================================
 * 9. AOCS_STATUS - the four enumerated states
 * ===================================================================== */

void test_aocs_status_has_four_ordered_values(void)
{
    TEST_ASSERT_EQUAL_INT(0, (int)BEACON_AOCS_OFF);
    TEST_ASSERT_EQUAL_INT(1, (int)BEACON_AOCS_DET);
    TEST_ASSERT_EQUAL_INT(2, (int)BEACON_AOCS_POINTING);
    TEST_ASSERT_EQUAL_INT(3, (int)BEACON_AOCS_FAULT);
}

/* Each of the four AOCS states survives pack -> unpack through bits 2-3. */
void test_aocs_status_all_four_values_round_trip(void)
{
    const beacon_aocs_status_t states[4] = {
        BEACON_AOCS_OFF, BEACON_AOCS_DET, BEACON_AOCS_POINTING, BEACON_AOCS_FAULT
    };
    int i;

    for (i = 0; i < 4; i++) {
        uint8_t status = beacon_status_pack(0u, 0u, states[i], 0u);
        beacon_aocs_status_t back = BEACON_AOCS_OFF;

        /* The 2-bit field is masked to bits 2-3 of the byte. */
        TEST_ASSERT_EQUAL_HEX8((uint8_t)((unsigned)i << 2), status);

        beacon_status_unpack(status, NULL, NULL, &back, NULL);
        TEST_ASSERT_EQUAL_INT((int)states[i], (int)back);
    }
}

/* A raw AOCS field value is always one of the four states, never a fifth. */
void test_aocs_status_unpack_never_exceeds_enum(void)
{
    unsigned v;

    for (v = 0u; v < 4u; v++) {
        beacon_aocs_status_t a = (beacon_aocs_status_t)0xFF;
        beacon_status_unpack((uint8_t)(v << 2), NULL, NULL, &a, NULL);
        TEST_ASSERT_TRUE(((int)a >= 0) && ((int)a <= 3));
    }
}

/* =====================================================================
 * 10. PAYLOADS bitfield - bit positions and round trip
 * ===================================================================== */

void test_payloads_bit_positions_are_literal(void)
{
    /* Bits 1-4 CLOUD_N_IMP: 0x1 -> 0x01, 0x8 -> 0x08 */
    TEST_ASSERT_EQUAL_HEX8(0x01u, beacon_payloads_pack(1u, 0u, 0u, 0u));
    TEST_ASSERT_EQUAL_HEX8(0x08u, beacon_payloads_pack(8u, 0u, 0u, 0u));
    /* Bits 5-6 CRY_STATUS: 1 -> 0x10, 2 -> 0x20, 3 -> 0x30 */
    TEST_ASSERT_EQUAL_HEX8(0x10u, beacon_payloads_pack(0u, 1u, 0u, 0u));
    TEST_ASSERT_EQUAL_HEX8(0x30u, beacon_payloads_pack(0u, 3u, 0u, 0u));
    /* Bit 7 HEATER_BAT -> 0x40 */
    TEST_ASSERT_EQUAL_HEX8(0x40u, beacon_payloads_pack(0u, 0u, 1u, 0u));
    /* Bit 8 HEATER_CRY -> 0x80 */
    TEST_ASSERT_EQUAL_HEX8(0x80u, beacon_payloads_pack(0u, 0u, 0u, 1u));
}

void test_payloads_round_trip_all_values(void)
{
    unsigned c, s, hb, hc;

    for (c = 0u; c < 16u; c++) {
        for (s = 0u; s < 4u; s++) {
            for (hb = 0u; hb < 2u; hb++) {
                for (hc = 0u; hc < 2u; hc++) {
                    uint8_t p = beacon_payloads_pack((uint8_t)c, (uint8_t)s,
                                                     (uint8_t)hb, (uint8_t)hc);
                    uint8_t rc = 0xEEu, rs = 0xEEu, rhb = 0xEEu, rhc = 0xEEu;

                    beacon_payloads_unpack(p, &rc, &rs, &rhb, &rhc);
                    TEST_ASSERT_EQUAL_UINT(c, (unsigned)rc);
                    TEST_ASSERT_EQUAL_UINT(s, (unsigned)rs);
                    TEST_ASSERT_EQUAL_UINT(hb, (unsigned)rhb);
                    TEST_ASSERT_EQUAL_UINT(hc, (unsigned)rhc);
                }
            }
        }
    }
}

void test_payloads_pack_masks_to_field_width(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x0Fu, beacon_payloads_pack(0xFFu, 0u, 0u, 0u));
    TEST_ASSERT_EQUAL_HEX8(0x30u, beacon_payloads_pack(0u, 0xFFu, 0u, 0u));
}

void test_payloads_unpack_accepts_null_outputs(void)
{
    uint8_t p = beacon_payloads_pack(3u, 1u, 1u, 1u);

    beacon_payloads_unpack(p, NULL, NULL, NULL, NULL);
    /* 3 | (1<<4) | (1<<6) | (1<<7) = 0xD3, unchanged by a NULL-output unpack. */
    TEST_ASSERT_EQUAL_HEX8(0xD3u, p);
}

/* =====================================================================
 * 11. Integration - the serialized frame carries the packed fields
 * ===================================================================== */

/* Byte 0 of the frame must be the STATUS byte and byte 6 the PAYLOADS byte:
 * unpacking them from the frame returns what went in. */
void test_frame_carries_the_packed_bitfields(void)
{
    beacon_hb_state_t st;
    uint8_t out[64];
    uint8_t b = 0u, l = 0u, o = 0u;
    beacon_aocs_status_t a = BEACON_AOCS_OFF;
    uint8_t c = 0u, s = 0u, hb = 0u, hc = 0u;

    fill_distinct(&st);
    (void)beacon_build_hb(&st, out, sizeof(out));

    beacon_status_unpack(out[0], &b, &l, &a, &o);
    TEST_ASSERT_EQUAL_UINT(st.bat_status, (unsigned)b);
    TEST_ASSERT_EQUAL_UINT(st.lines_fault, (unsigned)l);
    TEST_ASSERT_EQUAL_INT((int)st.aocs_status, (int)a);
    TEST_ASSERT_EQUAL_UINT(st.obc_status, (unsigned)o);

    beacon_payloads_unpack(out[6], &c, &s, &hb, &hc);
    TEST_ASSERT_EQUAL_UINT(st.cloud_n_imp, (unsigned)c);
    TEST_ASSERT_EQUAL_UINT(st.cry_status, (unsigned)s);
    TEST_ASSERT_EQUAL_UINT(st.heater_bat, (unsigned)hb);
    TEST_ASSERT_EQUAL_UINT(st.heater_cry, (unsigned)hc);
}
