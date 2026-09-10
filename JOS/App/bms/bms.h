#ifndef BMS_H
#define BMS_H

#include <stdbool.h>

#include "obsw_types.h"

/* Initialise BMS interface (brings up the subsystem SPI master to the EPS) */
void bms_init(void);

/* True once the subsystem SPI master to the EPS STM32L496 is initialised and
   ready. False means bms_get_status() is serving boot defaults, not EPS
   telemetry. */
bool bms_spi_ready(void);

/* Get current battery status */
bms_status_t bms_get_status(void);

/* Refresh the cached status from the EPS over the subsystem SPI bus.
 *
 * Returns 0 when fresh telemetry was read and cached (status.valid becomes
 * true), -1 otherwise — including while the EPS frame format is still a
 * pinned seam, see the note in bms.c. A -1 NEVER clears a previously valid
 * snapshot into a fabricated one: the cache either holds real telemetry or
 * reports itself invalid. */
int bms_poll(void);

/* ---------------------------------------------------------------------------
 * SoC threshold logic (SPF §3.6.1 / §1.5).
 *
 * SPF r.2135 states the split for this module explicitly:
 *     "bms.c/.h (EPS SPI interface, SoC threshold logic)"
 * so the band classification lives here rather than in the state machine.
 *
 * Threshold provenance (do not restate these as SPF values):
 *   - B_SCRIT ~25 % and B_OPOK ~80 % are SPF-anchored (r.787, r.1146, r.613).
 *   - B_CRIT and B_COMMOK are listed by the SPF (r.1146) WITHOUT a value:
 *     the document only calls them "low" and "intermediate". The numbers used
 *     by the OBSW are therefore PROJECT choices, and are passed in by the
 *     caller (bms_thresholds_t) instead of being baked in here, so the
 *     decision stays visible in one place.
 *
 * The classification is deliberately header-only (`static inline`): it must
 * be callable from both this module and App/obsw/state_machine.c without
 * adding a translation-unit dependency between them.
 * ------------------------------------------------------------------------- */

typedef enum {
    BMS_SOC_UNKNOWN = 0,  /* no valid telemetry: every SoC gate must CLOSE  */
    BMS_SOC_SCRIT,        /* <= B_SCRIT  (~25 %): immediate s2 (r.807)      */
    BMS_SOC_CRIT,         /* <= B_CRIT   (low)  : s2 (r.807)                */
    BMS_SOC_COMMOK,       /* <= B_COMMOK (int.) : s2 (r.808); s4->s2 r.658  */
    BMS_SOC_OK            /*  > B_COMMOK        : nominal                    */
} bms_soc_band_t;

/* Classify a snapshot. An invalid snapshot is always BMS_SOC_UNKNOWN,
 * whatever the cached byte happens to contain: a dead EPS link must never
 * read back as a healthy battery. */
static inline bms_soc_band_t bms_soc_band(const bms_status_t *s,
                                          const bms_thresholds_t *t)
{
    if ((s == NULL) || (t == NULL) || (!s->valid)) {
        return BMS_SOC_UNKNOWN;
    }
    if (s->soc <= t->b_scrit)  { return BMS_SOC_SCRIT;  }
    if (s->soc <= t->b_crit)   { return BMS_SOC_CRIT;   }
    if (s->soc <= t->b_commok) { return BMS_SOC_COMMOK; }
    return BMS_SOC_OK;
}

/* True only when a VALID snapshot is at or above B_OPOK. SPF r.658/r.824
 * require SoC >= B_OPOK for payload tasks and PDTs; an unknown SoC does not
 * satisfy that, so this returns false rather than assuming a full battery. */
static inline bool bms_soc_allows_payload(const bms_status_t *s,
                                          const bms_thresholds_t *t)
{
    if ((s == NULL) || (t == NULL) || (!s->valid)) {
        return false;
    }
    return (s->soc >= t->b_opok);
}

/* True for any band that the SPF maps to a transition into s2 (CRIT)
 * (r.600, r.807, r.808). BMS_SOC_UNKNOWN is NOT a low-battery condition:
 * "we cannot read the battery" must not be reported to ground as "the
 * battery is low". The protective behaviour for an unknown SoC is that the
 * SoC-GATED paths stay closed (bms_soc_allows_payload() == false), not that
 * the satellite claims a battery fault it cannot substantiate. */
static inline bool bms_soc_is_low(bms_soc_band_t band)
{
    return (band == BMS_SOC_SCRIT) ||
           (band == BMS_SOC_CRIT)  ||
           (band == BMS_SOC_COMMOK);
}

/* ---------------------------------------------------------------------------
 * EPS FDIR detectors (FDIR-EPS-EL-03 / -05 / -04)
 *
 * The charger-IC and battery-monitor fault detection and recovery decisions
 * are NOT here: they live in App/bms/eps_fdir.c/.h, host-unit-tested, driven
 * by a caller-supplied state snapshot (charging current + I2C read validity,
 * battery-monitor responsive flag, monotonic tick) and returning reset
 * requests the caller executes. They are separate from this file so the
 * decision logic stays testable on the host without mirroring the CubeMX SPI
 * struct layout that keeps bms.c flight-only (see the note in
 * test/project.yml). This header keeps the SoC band classification and the
 * existing thresholds exactly as they were.
 * ------------------------------------------------------------------------- */

#endif /* BMS_H */
