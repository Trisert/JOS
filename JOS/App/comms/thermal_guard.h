#ifndef THERMAL_GUARD_H
#define THERMAL_GUARD_H

#include <stdint.h>
#include <stdbool.h>

/* thermal_guard — FDIR-COMM-EL-02 LoRa transceiver over-temperature guard.
 *
 * Source: RED_FDIR_V2.xlsx, sheet 'COMM', FMEA-COMM-EL-02 / FDIR-COMM-EL-02
 * (SYS SPF V3 / ANNEXES / ANNEX 09). The document has NO release status and
 * carries TBD fields elsewhere: it is the best available evidence, not an
 * approved baseline (see /root/sp_findings/TROVATO.md).
 *
 * What the document pins down (NOT chosen here):
 *   - feared event : "LoRa transceiver overheating"
 *   - protection   : "Thermal monitoring" (software)
 *   - entry        : "LoRa temperature above set point"
 *   - exit         : "LoRa temperature below set point"
 *   - recovery     : "Stop Lora TX"
 *   - modes        : "All modes"  (this guard is therefore NOT gated on the
 *                    operational state; thermal_guard_update() takes no mode)
 *   - monitoring   : "HK telemetry monitoring"
 *
 * The document says "set point" once, but uses it for BOTH the entry and the
 * exit condition. A single threshold makes the TX command chatter on and off
 * at that value (every sample alternating sides of it toggles the radio),
 * which is exactly the output oscillation this guard exists to prevent. Two
 * distinct set points with a dead band are therefore used, and the band is a
 * PROJECT CHOICE declared in thermal_guard_default_config() — the document
 * gives no number for either set point.
 *
 * Temperature unit: 0.1 C (int16_t), matching the housekeeping temperature
 * convention in JOS (bms_status_t.temp_c, SW_DATA_TYPES TEMP_* fields).
 */

typedef struct {
    int16_t entry_setpoint;  /* 0.1 C: T >= entry  -> assert stop TX */
    int16_t exit_setpoint;   /* 0.1 C: T <= exit   -> release TX     */
} thermal_guard_config_t;

/* Reset to the declared defaults, TX allowed, not overheated. */
void thermal_guard_init(void);

/* The declared defaults, with rationale (see the .c file). */
const thermal_guard_config_t *thermal_guard_default_config(void);

/* Override the configuration. Rejected (returns false, config unchanged) when
 * the band would be empty or inverted (entry_setpoint <= exit_setpoint): a
 * non-hysteretic or reversed pair is a configuration error, not a working
 * guard, so it is refused rather than silently accepted. */
bool thermal_guard_configure(const thermal_guard_config_t *cfg);

/* Feed one LoRa temperature sample (0.1 C). Returns true while TX is allowed
 * (i.e. NOT `stop TX`); false when the over-temperature reaction is active. */
bool thermal_guard_update(int16_t lora_temp);

/* True while "Stop Lora TX" is commanded (temperature at/above entry, until it
 * falls to/below exit). */
bool thermal_guard_tx_stop_requested(void);

/* True while TX is permitted (the negation of the above, kept for callers
 * that gate a transmit call directly). */
bool thermal_guard_tx_allowed(void);

/* The configuration in force (never NULL). */
const thermal_guard_config_t *thermal_guard_config(void);

#endif /* THERMAL_GUARD_H */
