#include "sim_scenarios.h"
#include "sim_protocol.h"
#include "esp_log.h"
#include <string.h>
#include <string.h>

static const char *SC_TAG = "SCENARIO";

static sim_state_t *g_state;

static void nominal_reset(void);
static void nominal_start(void);
static void nominal_step(uint32_t dt_ms);

static void lowbat_reset(void);
static void lowbat_start(void);
static void lowbat_step(uint32_t dt_ms);

static void debris_reset(void);
static void debris_start(void);
static void debris_step(uint32_t dt_ms);

static void full_orbit_reset(void);
static void full_orbit_start(void);
static void full_orbit_step(uint32_t dt_ms);

static sim_scenario_t nominal_sc = {
    .name = "nominal",
    .desc = "Normal satellite operations: boot, ready, activate payload",
    .reset = nominal_reset,
    .start = nominal_start,
    .step = nominal_step,
};

static sim_scenario_t lowbat_sc = {
    .name = "low_battery",
    .desc = "Battery drain to critical, recovery cycle",
    .reset = lowbat_reset,
    .start = lowbat_start,
    .step = lowbat_step,
};

static sim_scenario_t debris_sc = {
    .name = "debris_impact",
    .desc = "Simulate debris impact on CLOUD faces",
    .reset = debris_reset,
    .start = debris_start,
    .step = debris_step,
};

static sim_scenario_t full_orbit_sc = {
    .name = "full_orbit",
    .desc = "Full 90-min orbit: eclipse, sunlight, payload ops",
    .reset = full_orbit_reset,
    .start = full_orbit_start,
    .step = full_orbit_step,
};

sim_scenario_t *sim_scenario_list[] = {
    &nominal_sc,
    &lowbat_sc,
    &debris_sc,
    &full_orbit_sc,
    NULL,
};

void sim_scenarios_init(sim_state_t *state)
{
    g_state = state;
    for (int i = 0; sim_scenario_list[i]; i++) {
        sim_scenario_list[i]->state = state;
    }
}

sim_scenario_t *sim_scenario_find(const char *name)
{
    for (int i = 0; sim_scenario_list[i]; i++) {
        if (strcasecmp(sim_scenario_list[i]->name, name) == 0) {
            return sim_scenario_list[i];
        }
    }
    return NULL;
}

static void nominal_reset(void)
{
    nominal_sc.elapsed_ms = 0;
    nominal_sc.phase = 0;
    nominal_sc.active = false;
}

static void nominal_start(void)
{
    nominal_reset();
    nominal_sc.active = true;
    ESP_LOGI(SC_TAG, "NOMINAL scenario started");
}

static void nominal_step(uint32_t dt_ms)
{
    if (!nominal_sc.active) return;
    nominal_sc.elapsed_ms += dt_ms;
    uint32_t t = nominal_sc.elapsed_ms;

    if (t < 2000 && nominal_sc.phase == 0) {
        nominal_sc.phase = 1;
        ESP_LOGI(SC_TAG, "[NOMINAL] Phase 1: Boot sequence");
    }
    if (t >= 5000 && t < 7000 && nominal_sc.phase == 1) {
        nominal_sc.phase = 2;
        ESP_LOGI(SC_TAG, "[NOMINAL] Phase 2: Sending ACTIVATE_PAYLOAD");
        uint8_t cmd = 0x05;
        uart_write_bytes(UART_NUM_1, &cmd, 1);
    }
    if (t >= 15000 && nominal_sc.phase == 2) {
        nominal_sc.phase = 3;
        ESP_LOGI(SC_TAG, "[NOMINAL] Phase 3: Payload active, monitoring");
    }
    if (t >= 30000 && nominal_sc.phase == 3) {
        nominal_sc.phase = 4;
        nominal_sc.active = false;
        ESP_LOGI(SC_TAG, "[NOMINAL] Scenario complete");
    }
}

static void lowbat_reset(void)
{
    lowbat_sc.elapsed_ms = 0;
    lowbat_sc.phase = 0;
    lowbat_sc.active = false;
    if (g_state) {
        g_state->bms.soc = 100;
        g_state->bms.voltage_mv = 4200;
        g_state->drain_rate = 0;
    }
}

static void lowbat_start(void)
{
    lowbat_reset();
    lowbat_sc.active = true;
    if (g_state) {
        g_state->bms.soc = 85;
        g_state->drain_rate = 5.0f;
    }
    ESP_LOGI(SC_TAG, "LOW_BATTERY scenario started (drain: 5%%/min)");
}

static void lowbat_step(uint32_t dt_ms)
{
    if (!lowbat_sc.active || !g_state) return;
    lowbat_sc.elapsed_ms += dt_ms;
    uint32_t t = lowbat_sc.elapsed_ms;

    if (t >= 12000 && t < 14000 && lowbat_sc.phase == 0) {
        lowbat_sc.phase = 1;
        g_state->drain_rate = 0;
        ESP_LOGI(SC_TAG, "[LOWBAT] Phase 1: Stopped drain at %d%%, waiting for CRIT",
                 g_state->bms.soc);
    }

    if (g_state->bms.soc <= 80 && lowbat_sc.phase == 0) {
        lowbat_sc.phase = 1;
        g_state->drain_rate = 0;
        ESP_LOGI(SC_TAG, "[LOWBAT] Battery reached %d%%, observing state machine",
                 g_state->bms.soc);
    }

    if (t >= 25000 && lowbat_sc.phase == 1) {
        lowbat_sc.phase = 2;
        g_state->drain_rate = -3.0f;
        ESP_LOGI(SC_TAG, "[LOWBAT] Phase 2: Charging at 3%%/min");
    }

    if (g_state->bms.soc >= 80 && lowbat_sc.phase == 2) {
        lowbat_sc.phase = 3;
        g_state->drain_rate = 0;
        ESP_LOGI(SC_TAG, "[LOWBAT] Phase 3: Battery recovered to %d%%",
                 g_state->bms.soc);
    }

    if (lowbat_sc.phase >= 3 && t >= 35000) {
        lowbat_sc.active = false;
        ESP_LOGI(SC_TAG, "[LOWBAT] Scenario complete");
    }
}

static void debris_reset(void)
{
    debris_sc.elapsed_ms = 0;
    debris_sc.phase = 0;
    debris_sc.active = false;
}

static void debris_start(void)
{
    debris_reset();
    debris_sc.active = true;
    ESP_LOGI(SC_TAG, "DEBRIS_IMPACT scenario started");
}

static void debris_step(uint32_t dt_ms)
{
    if (!debris_sc.active || !g_state) return;
    debris_sc.elapsed_ms += dt_ms;
    uint32_t t = debris_sc.elapsed_ms;

    if (t >= 3000 && t < 5000 && debris_sc.phase == 0) {
        debris_sc.phase = 1;
        ESP_LOGI(SC_TAG, "[DEBRIS] Impact! Modifying CLOUD face 0 stripe 4-6");
        g_state->cloud_adc.channels[4] = 3500;
        g_state->cloud_adc.channels[5] = 3800;
        g_state->cloud_adc.channels[6] = 200;
    }

    if (t >= 10000 && debris_sc.phase == 1) {
        debris_sc.phase = 2;
        ESP_LOGI(SC_TAG, "[DEBRIS] Partial recovery on face 0");
        g_state->cloud_adc.channels[4] = 2200;
    }

    if (t >= 20000 && debris_sc.phase == 2) {
        debris_sc.active = false;
        ESP_LOGI(SC_TAG, "[DEBRIS] Scenario complete");
    }
}

static void full_orbit_reset(void)
{
    full_orbit_sc.elapsed_ms = 0;
    full_orbit_sc.phase = 0;
    full_orbit_sc.active = false;
    if (g_state) {
        g_state->bms.soc = 95;
        g_state->bms.voltage_mv = 4150;
        g_state->drain_rate = 0;
    }
}

static void full_orbit_start(void)
{
    full_orbit_reset();
    full_orbit_sc.active = true;
    if (g_state) {
        g_state->drain_rate = 2.0f;
    }
    ESP_LOGI(SC_TAG, "FULL_ORBIT scenario started (90 min, 10x speed = 9 min real time)");
}

static void full_orbit_step(uint32_t dt_ms)
{
    if (!full_orbit_sc.active || !g_state) return;
    full_orbit_sc.elapsed_ms += dt_ms;

    uint32_t orbit_ms = full_orbit_sc.elapsed_ms;
    uint32_t eclipse_start = 20000;
    uint32_t eclipse_end   = 40000;
    uint32_t payload_start = 50000;

    if (orbit_ms >= eclipse_start && orbit_ms < eclipse_end && full_orbit_sc.phase == 0) {
        full_orbit_sc.phase = 1;
        g_state->drain_rate = 8.0f;
        g_state->adc.ch4_mv = 0;
        g_state->adc.ch5_mv = 0;
        ESP_LOGI(SC_TAG, "[ORBIT] Eclipse phase - solar panels dark");
    }

    if (orbit_ms >= eclipse_end && full_orbit_sc.phase == 1) {
        full_orbit_sc.phase = 2;
        g_state->drain_rate = -5.0f;
        g_state->adc.ch4_mv = 1500;
        g_state->adc.ch5_mv = 1200;
        ESP_LOGI(SC_TAG, "[ORBIT] Sunlight phase - charging");
    }

    if (orbit_ms >= payload_start && full_orbit_sc.phase == 2) {
        full_orbit_sc.phase = 3;
        g_state->drain_rate = 1.0f;
        ESP_LOGI(SC_TAG, "[ORBIT] Payload operations phase");
    }

    if (full_orbit_sc.elapsed_ms >= 5400000) {
        full_orbit_sc.active = false;
        ESP_LOGI(SC_TAG, "[ORBIT] Full orbit complete. Final SoC: %d%%",
                 g_state->bms.soc);
    }
}
