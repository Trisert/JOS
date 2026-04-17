#ifndef SIM_SENSORS_H
#define SIM_SENSORS_H

#include "sim_state.h"

void sim_sensors_init(sim_state_t *state);

sim_imu_data_t sim_sensors_generate_imu(float t);
sim_mag_data_t sim_sensors_generate_mag(float t);
sim_adc_data_t sim_sensors_generate_adc(float t, uint8_t crystals_en);
sim_cloud_adc_t sim_sensors_generate_cloud_adc(float t);

#endif
