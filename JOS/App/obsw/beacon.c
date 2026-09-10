/* ---------------------------------------------------------------------------
 * beacon.c - housekeeping beacon composition (byte-exact HB frame)
 *
 * See beacon.h for the layout source (SW_DATA_TYPES.xlsx sheet "HB"), the
 * width correction for the GPS components, and the two documented assumptions
 * (bit order and byte order) that the document leaves open.
 *
 * This module is pure C: no HAL, no RTOS, no global state. It is therefore
 * host-compilable and is linked into the Ceedling unit-test build alongside
 * the flight build (JOS/test/project.yml already has ../App/obsw on :source:).
 *
 * Refs: ECSS-E-ST-40C 5.5 (validation), JPL-182 Rule 31 (all code exercised by
 *       tests).
 * ------------------------------------------------------------------------- */
#include "beacon.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Little-endian placement helpers.
 *
 * Written byte-by-byte rather than with a struct/memcpy on purpose: the
 * on-wire order is then a property of this code and identical on the STM32
 * target and on the x86/aarch64 host under test. The document does not state
 * an endianness (beacon.h), so keeping it explicit is what makes the choice
 * reviewable and changeable in one place.
 * ------------------------------------------------------------------------- */
static void beacon_put_u16_le(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void beacon_put_u32_le(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ---------------------------------------------------------------------------
 * STATUS / PAYLOADS bitfield helpers
 * ------------------------------------------------------------------------- */

uint8_t beacon_status_pack(uint8_t bat_status,
                           uint8_t lines_fault,
                           beacon_aocs_status_t aocs_status,
                           uint8_t obc_status)
{
    uint8_t out = 0u;

    out |= (uint8_t)((bat_status & BEACON_STATUS_BAT_STATUS_MASK)
                     << BEACON_STATUS_BAT_STATUS_SHIFT);
    out |= (uint8_t)((lines_fault & BEACON_STATUS_LINES_FAULT_MASK)
                     << BEACON_STATUS_LINES_FAULT_SHIFT);
    out |= (uint8_t)(((uint8_t)aocs_status & BEACON_STATUS_AOCS_STATUS_MASK)
                     << BEACON_STATUS_AOCS_STATUS_SHIFT);
    out |= (uint8_t)((obc_status & BEACON_STATUS_OBC_STATUS_MASK)
                     << BEACON_STATUS_OBC_STATUS_SHIFT);

    return out;
}

void beacon_status_unpack(uint8_t status,
                          uint8_t *bat_status,
                          uint8_t *lines_fault,
                          beacon_aocs_status_t *aocs_status,
                          uint8_t *obc_status)
{
    if (bat_status != NULL) {
        *bat_status = (uint8_t)((status >> BEACON_STATUS_BAT_STATUS_SHIFT)
                                & BEACON_STATUS_BAT_STATUS_MASK);
    }
    if (lines_fault != NULL) {
        *lines_fault = (uint8_t)((status >> BEACON_STATUS_LINES_FAULT_SHIFT)
                                 & BEACON_STATUS_LINES_FAULT_MASK);
    }
    if (aocs_status != NULL) {
        /* The 2-bit field only ever holds the four enumerated values, so the
         * cast is total: no out-of-range AOCS state can be produced. */
        *aocs_status = (beacon_aocs_status_t)((status >> BEACON_STATUS_AOCS_STATUS_SHIFT)
                                              & BEACON_STATUS_AOCS_STATUS_MASK);
    }
    if (obc_status != NULL) {
        *obc_status = (uint8_t)((status >> BEACON_STATUS_OBC_STATUS_SHIFT)
                                & BEACON_STATUS_OBC_STATUS_MASK);
    }
}

uint8_t beacon_payloads_pack(uint8_t cloud_n_imp,
                             uint8_t cry_status,
                             uint8_t heater_bat,
                             uint8_t heater_cry)
{
    uint8_t out = 0u;

    out |= (uint8_t)((cloud_n_imp & BEACON_PAYLOADS_CLOUD_N_IMP_MASK)
                     << BEACON_PAYLOADS_CLOUD_N_IMP_SHIFT);
    out |= (uint8_t)((cry_status & BEACON_PAYLOADS_CRY_STATUS_MASK)
                     << BEACON_PAYLOADS_CRY_STATUS_SHIFT);
    out |= (uint8_t)((heater_bat & BEACON_PAYLOADS_HEATER_BAT_MASK)
                     << BEACON_PAYLOADS_HEATER_BAT_SHIFT);
    out |= (uint8_t)((heater_cry & BEACON_PAYLOADS_HEATER_CRY_MASK)
                     << BEACON_PAYLOADS_HEATER_CRY_SHIFT);

    return out;
}

void beacon_payloads_unpack(uint8_t payloads,
                            uint8_t *cloud_n_imp,
                            uint8_t *cry_status,
                            uint8_t *heater_bat,
                            uint8_t *heater_cry)
{
    if (cloud_n_imp != NULL) {
        *cloud_n_imp = (uint8_t)((payloads >> BEACON_PAYLOADS_CLOUD_N_IMP_SHIFT)
                                 & BEACON_PAYLOADS_CLOUD_N_IMP_MASK);
    }
    if (cry_status != NULL) {
        *cry_status = (uint8_t)((payloads >> BEACON_PAYLOADS_CRY_STATUS_SHIFT)
                                & BEACON_PAYLOADS_CRY_STATUS_MASK);
    }
    if (heater_bat != NULL) {
        *heater_bat = (uint8_t)((payloads >> BEACON_PAYLOADS_HEATER_BAT_SHIFT)
                                & BEACON_PAYLOADS_HEATER_BAT_MASK);
    }
    if (heater_cry != NULL) {
        *heater_cry = (uint8_t)((payloads >> BEACON_PAYLOADS_HEATER_CRY_SHIFT)
                                & BEACON_PAYLOADS_HEATER_CRY_MASK);
    }
}

/* ---------------------------------------------------------------------------
 * Serialization
 * ------------------------------------------------------------------------- */

size_t beacon_build_hb(const beacon_hb_state_t *st, uint8_t *out, size_t out_len)
{
    if (st == NULL || out == NULL || out_len < (size_t)BEACON_HB_LEN) {
        return 0u;
    }

    /* Zero the whole frame first: every byte the sheet leaves blank
     * (row 1 byte 8, rows 2-4) is emitted as 0x00, and no byte can be left
     * carrying whatever the caller's buffer held. */
    (void)memset(out, 0, (size_t)BEACON_HB_LEN);

    /* Row 1 */
    out[BEACON_OFF_STATUS]    = beacon_status_pack(st->bat_status,
                                                   st->lines_fault,
                                                   st->aocs_status,
                                                   st->obc_status);
    out[BEACON_OFF_I_BAT_CHG] = st->i_bat_chg;
    out[BEACON_OFF_I_BAT_DSG] = st->i_bat_dsg;
    out[BEACON_OFF_SC_CHG]    = st->sc_chg;
    out[BEACON_OFF_V_BAT]     = st->v_bat;
    out[BEACON_OFF_BAT_SOC]   = st->bat_soc;
    out[BEACON_OFF_PAYLOADS]  = beacon_payloads_pack(st->cloud_n_imp,
                                                     st->cry_status,
                                                     st->heater_bat,
                                                     st->heater_cry);
    /* Row 2-4: reserved, left zero by the memset. */

    /* Row 5 - pass-through 32-bit value, no format conversion. */
    beacon_put_u32_le(&out[BEACON_OFF_GPS_TIMESTAMP], st->gps_timestamp);

    /* Row 6 */
    out[BEACON_OFF_TEMP_1]     = st->temp_1;
    out[BEACON_OFF_TEMP_2]     = st->temp_2;
    out[BEACON_OFF_TEMP_3]     = st->temp_3;
    out[BEACON_OFF_TEMP_4]     = st->temp_4;
    out[BEACON_OFF_TEMP_EXT_1] = st->temp_ext_1;
    out[BEACON_OFF_TEMP_EXT_2] = st->temp_ext_2;
    out[BEACON_OFF_TEMP_EXT_3] = st->temp_ext_3;
    out[BEACON_OFF_TEMP_EXT_4] = st->temp_ext_4;

    /* Row 7 */
    out[BEACON_OFF_TEMP_EXT_5] = st->temp_ext_5;
    out[BEACON_OFF_TEMP_EXT_6] = st->temp_ext_6;
    out[BEACON_OFF_BAT_TEMP]   = st->bat_temp;
    out[BEACON_OFF_CRY_TEMP]   = st->cry_temp;
    beacon_put_u16_le(&out[BEACON_OFF_GPS_X], st->gps_x);
    beacon_put_u16_le(&out[BEACON_OFF_GPS_Y], st->gps_y);

    /* Row 8 */
    beacon_put_u16_le(&out[BEACON_OFF_GPS_Z],     st->gps_z);
    beacon_put_u16_le(&out[BEACON_OFF_GPS_X_DOT], st->gps_x_dot);
    beacon_put_u16_le(&out[BEACON_OFF_GPS_Y_DOT], st->gps_y_dot);
    beacon_put_u16_le(&out[BEACON_OFF_GPS_Z_DOT], st->gps_z_dot);

    return (size_t)BEACON_HB_LEN;
}
