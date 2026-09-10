/**
 * @file    test_thermal_guard.c
 * @brief   Unit tests for the FDIR-COMM-EL-02 LoRa over-temperature guard
 *          (App/comms/thermal_guard.c, RED_FDIR_V2.xlsx sheet 'COMM',
 *          FMEA-COMM-EL-02).
 *
 * The document gives one "set point" for both the entry ("LoRa temperature
 * above set point") and the exit ("LoRa temperature below set point")
 * condition, and no number. A single threshold applied to both directions
 * toggles the stop-TX command on every sample that crosses it — the output
 * oscillates around the threshold. These tests pin the hysteresis: the command
 * must hold its state for any temperature strictly inside the band, and only
 * the threshold that matches the current state may change it.
 *
 * The values themselves are declared project choices, so the tests assert the
 * behaviour (hysteresis, polarity, rejection of a non-hysteretic config), not
 * that a particular number is correct.
 */

#include "unity.h"
#include "thermal_guard.h"

void setUp(void)
{
    thermal_guard_init();
}

void tearDown(void)
{
}

/* The guard must be hysteretic out of the box: a real dead band between the
 * entry and the exit set point, not one value used twice. */
void test_defaults_declare_a_real_dead_band(void)
{
    const thermal_guard_config_t *cfg = thermal_guard_default_config();

    TEST_ASSERT_NOT_NULL(cfg);
    TEST_ASSERT_GREATER_THAN_INT16(cfg->exit_setpoint, cfg->entry_setpoint);
    TEST_ASSERT_TRUE(thermal_guard_tx_allowed());
    TEST_ASSERT_FALSE(thermal_guard_tx_stop_requested());
}

/* Entry: "LoRa temperature above set point" asserts the stop-TX command at the
 * entry set point and above, not below it. */
void test_entry_asserts_stop_at_setpoint(void)
{
    const thermal_guard_config_t *cfg = thermal_guard_config();

    TEST_ASSERT_TRUE(thermal_guard_update((int16_t)(cfg->entry_setpoint - 1)));
    TEST_ASSERT_FALSE(thermal_guard_tx_stop_requested());

    TEST_ASSERT_FALSE(thermal_guard_update(cfg->entry_setpoint));
    TEST_ASSERT_TRUE(thermal_guard_tx_stop_requested());
    TEST_ASSERT_FALSE(thermal_guard_tx_allowed());
}

/* Exit: the stop is released only once the temperature reaches the exit set
 * point from above. */
void test_exit_releases_only_at_exit_setpoint(void)
{
    const thermal_guard_config_t *cfg = thermal_guard_config();

    TEST_ASSERT_FALSE(thermal_guard_update(cfg->entry_setpoint));
    TEST_ASSERT_TRUE(thermal_guard_tx_stop_requested());

    TEST_ASSERT_FALSE(thermal_guard_update((int16_t)(cfg->exit_setpoint + 1)));
    TEST_ASSERT_TRUE(thermal_guard_tx_stop_requested());

    TEST_ASSERT_TRUE(thermal_guard_update(cfg->exit_setpoint));
    TEST_ASSERT_FALSE(thermal_guard_tx_stop_requested());
}

/* The core requirement: no oscillation around the threshold. Once the
 * temperature is above the entry set point, any sample that stays above the
 * exit set point — including samples that dip back below the entry set point —
 * must leave the stop-TX command asserted. Likewise the reverse.
 *
 * This is the sequence that a single-threshold implementation fails: the
 * samples bounce across the entry set point while remaining above the exit. */
void test_hysteresis_does_not_oscillate_in_the_band(void)
{
    const thermal_guard_config_t *cfg = thermal_guard_config();
    int16_t entry = cfg->entry_setpoint;
    int16_t exit  = cfg->exit_setpoint;
    int16_t seq[] = {
        entry,                  /* trip                            */
        entry - 1, entry - 5,   /* back below entry, above exit    */
        (int16_t)(exit + 1),    /* bottom of the band              */
        entry - 1, entry,       /* up again across the entry point */
        (int16_t)(exit + 2),
    };
    uint32_t i;

    for (i = 0u; i < (sizeof(seq) / sizeof(seq[0])); i++) {
        thermal_guard_update(seq[i]);
        /* Every sample in the sequence is inside [exit, ...], so once tripped
         * the command never releases and never flaps. */
        TEST_ASSERT_TRUE(thermal_guard_tx_stop_requested());
        TEST_ASSERT_FALSE(thermal_guard_tx_allowed());
    }

    /* And the moment it drops below the exit set point it releases, once. */
    TEST_ASSERT_TRUE(thermal_guard_update((int16_t)(exit - 1)));
    TEST_ASSERT_FALSE(thermal_guard_tx_stop_requested());

    /* Below the entry set point it must not re-trip on the way back up. */
    thermal_guard_update((int16_t)(entry - 1));
    TEST_ASSERT_FALSE(thermal_guard_tx_stop_requested());
    TEST_ASSERT_TRUE(thermal_guard_update((int16_t)(exit + 1)));
    TEST_ASSERT_FALSE(thermal_guard_tx_stop_requested());
}

/* Long alternating run across the entry set point while staying in the band:
 * the number of state changes must be exactly one (the trip) — an oscillating
 * implementation would flip on every crossing. */
void test_no_flapping_over_many_crossings(void)
{
    const thermal_guard_config_t *cfg = thermal_guard_config();
    int16_t entry = cfg->entry_setpoint;
    int16_t low_in_band = (int16_t)(cfg->exit_setpoint + 1);
    int transitions = 0;
    bool  prev = thermal_guard_tx_stop_requested();
    int   i;

    for (i = 0; i < 500; i++) {
        bool now;
        thermal_guard_update((i % 2) ? low_in_band : (int16_t)(entry + 5));
        now = thermal_guard_tx_stop_requested();
        if (now != prev) {
            transitions++;
        }
        prev = now;
    }

    TEST_ASSERT_EQUAL_INT(1, transitions);     /* tripped once, never flapped */
    TEST_ASSERT_TRUE(thermal_guard_tx_stop_requested());
}

/* A configuration without a dead band (entry <= exit) is a malformed guard and
 * must be refused instead of silently accepted. */
void test_non_hysteretic_config_is_rejected(void)
{
    const thermal_guard_config_t *before = thermal_guard_config();
    thermal_guard_config_t cfg = *before;
    int16_t entry_before = before->entry_setpoint;

    cfg.entry_setpoint = 500;
    cfg.exit_setpoint  = 500;      /* equal: band of zero width */
    TEST_ASSERT_FALSE(thermal_guard_configure(&cfg));

    cfg.entry_setpoint = 400;
    cfg.exit_setpoint  = 500;      /* inverted */
    TEST_ASSERT_FALSE(thermal_guard_configure(&cfg));

    TEST_ASSERT_FALSE(thermal_guard_configure(NULL));

    /* Unchanged. */
    TEST_ASSERT_EQUAL_INT16(entry_before, thermal_guard_config()->entry_setpoint);
}

/* A valid override moves both thresholds (it is used, not ignored). */
void test_valid_config_is_applied(void)
{
    thermal_guard_config_t cfg = { 700, 650 };

    TEST_ASSERT_TRUE(thermal_guard_configure(&cfg));
    TEST_ASSERT_EQUAL_INT16(700, thermal_guard_config()->entry_setpoint);
    TEST_ASSERT_EQUAL_INT16(650, thermal_guard_config()->exit_setpoint);

    TEST_ASSERT_TRUE(thermal_guard_update(699));
    TEST_ASSERT_FALSE(thermal_guard_tx_stop_requested());

    TEST_ASSERT_FALSE(thermal_guard_update(700));   /* trip at the new entry */
    TEST_ASSERT_TRUE(thermal_guard_tx_stop_requested());
    TEST_ASSERT_FALSE(thermal_guard_tx_allowed());

    TEST_ASSERT_FALSE(thermal_guard_update(651));   /* still in the new band */
    TEST_ASSERT_TRUE(thermal_guard_tx_stop_requested());

    TEST_ASSERT_TRUE(thermal_guard_update(650));    /* release at new exit  */
    TEST_ASSERT_FALSE(thermal_guard_tx_stop_requested());
}
