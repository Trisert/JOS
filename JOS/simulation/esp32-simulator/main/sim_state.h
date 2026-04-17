#ifndef SIM_STATE_H
#define SIM_STATE_H

#include <stdint.h>
#include "sim_protocol.h"

typedef struct {
    sim_eps_telemetry_t eps;
    sim_aocs_telemetry_t aocs;
    sim_aocs_telemetry_t aocs_redundant;
    sim_cloud_data_t cloud;
    sim_payload_gpio_t payload_gpio;
    sim_temp_data_t temps;
    sim_pok_state_t pok;
    sim_gpio_expander_t gpio_exp;
    bool running;
    uint32_t time_multiplier;
    bool auto_mode;
    float drain_rate;
    uint32_t boot_tick_ms;
} sim_state_t;

#endif
