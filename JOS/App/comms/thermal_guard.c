/* thermal_guard.c — FDIR-COMM-EL-02 LoRa over-temperature guard (see header). */

#include "thermal_guard.h"

#include <stddef.h>   /* NULL */

/* Defaults: the numbers RED_FDIR_V2.xlsx does not give.
 *
 *   entry_setpoint = 600 (60.0 C) <- CHOICE. The SX1268 is rated for operation
 *                                    up to +85 C (datasheet "Operating
 *                                    temperature range"). 60 C keeps ~25 C of
 *                                    margin before the rating, so TX is cut
 *                                    while the die still has headroom and the
 *                                    radio can recover by itself. The document
 *                                    says "set point" but gives no value.
 *   exit_setpoint  = 500 (50.0 C) <- CHOICE. A 10 C dead band. Wider than the
 *                                    HK sampling noise on the on-board
 *                                    temperature telemetry, narrower than the
 *                                    25 C margin, so recovery is not delayed
 *                                    until the radio is nearly hot again.
 */
static const thermal_guard_config_t thermal_guard_defaults = {
    600,   /* entry_setpoint : 60.0 C */
    500    /* exit_setpoint  : 50.0 C */
};

static thermal_guard_config_t s_cfg;
static bool s_stop_tx;

const thermal_guard_config_t *thermal_guard_default_config(void)
{
    return &thermal_guard_defaults;
}

const thermal_guard_config_t *thermal_guard_config(void)
{
    return &s_cfg;
}

bool thermal_guard_configure(const thermal_guard_config_t *cfg)
{
    if ((cfg == NULL) || (cfg->entry_setpoint <= cfg->exit_setpoint)) {
        return false;   /* empty or inverted band: no hysteresis possible */
    }
    s_cfg = *cfg;
    return true;
}

void thermal_guard_init(void)
{
    s_cfg     = thermal_guard_defaults;
    s_stop_tx = false;
}

bool thermal_guard_update(int16_t lora_temp)
{
    /* Hysteresis: only the threshold that matches the current state is
     * evaluated, so a sample inside the band leaves the output where it was.
     * This is what stops the stop-TX command oscillating around one point. */
    if (s_stop_tx) {
        if (lora_temp <= s_cfg.exit_setpoint) {
            s_stop_tx = false;   /* cooled to/below the exit set point */
        }
    } else {
        if (lora_temp >= s_cfg.entry_setpoint) {
            s_stop_tx = true;    /* heated to/above the entry set point */
        }
    }
    return !s_stop_tx;
}

bool thermal_guard_tx_stop_requested(void)
{
    return s_stop_tx;
}

bool thermal_guard_tx_allowed(void)
{
    return !s_stop_tx;
}
