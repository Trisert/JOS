#ifndef SIM_SENSORS_H
#define SIM_SENSORS_H

#include "sim_state.h"

void sim_sensors_init(sim_state_t *state);

sim_aocs_telemetry_t sim_sensors_generate_aocs(float t);
sim_eps_telemetry_t sim_sensors_generate_eps(float t, uint8_t soc);
sim_cloud_data_t sim_sensors_generate_cloud(float t, uint8_t crystals_en);
sim_temp_data_t sim_sensors_generate_temps(float t);

#endif
