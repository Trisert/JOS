#ifndef SIM_STATE_H
#define SIM_STATE_H

#include <stdint.h>
#include "sim_protocol.h"

typedef struct {
    sim_bms_data_t bms;
    sim_imu_data_t imu;
    sim_mag_data_t mag;
    sim_adc_data_t adc;
    sim_cloud_adc_t cloud_adc;
    bool running;
    uint32_t time_multiplier;
    bool auto_mode;
    float drain_rate;
    uint32_t boot_tick_ms;
} sim_state_t;

#endif
