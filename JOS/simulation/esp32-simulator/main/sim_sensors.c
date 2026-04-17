#include "sim_sensors.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static sim_state_t *s_state;

#define EPS_V_FULL_MV     8400
#define EPS_V_EMPTY_MV    6000
#define EPS_V_RANGE_MV    (EPS_V_FULL_MV - EPS_V_EMPTY_MV)

static uint16_t soc_to_voltage_mv(uint8_t soc)
{
    return (uint16_t)(EPS_V_EMPTY_MV + ((uint32_t)soc * EPS_V_RANGE_MV) / 100);
}

void sim_sensors_init(sim_state_t *state)
{
    s_state = state;
    s_state->boot_tick_ms = 0;

    for (int i = 0; i < 16; i++) {
        s_state->cloud.channels[i] = 2048;
    }
    s_state->cloud.face = 0;

    for (int i = 0; i < 4; i++) {
        s_state->temps.temps[i] = 200;
    }
}

sim_eps_telemetry_t sim_sensors_generate_eps(float t, uint8_t soc)
{
    sim_eps_telemetry_t d;
    float w = 0.005f;

    d.soc = soc;
    d.soh = 95 + (uint8_t)(3.0f * sinf(2.0f * M_PI * 0.0001f * t));
    d.temp_c = (int16_t)(200.0f + 40.0f * sinf(2.0f * M_PI * w * t));
    d.voltage_mv = soc_to_voltage_mv(soc);
    d.current_ma = (int16_t)(300.0f + 150.0f * sinf(2.0f * M_PI * 0.002f * t));
    d.flags = 0;

    if (soc < 10) {
        d.flags |= 0x0001;
    }
    if (d.temp_c > 450) {
        d.flags |= 0x0002;
    }
    if (soc < 5) {
        d.flags |= 0x0004;
    }

    return d;
}

sim_aocs_telemetry_t sim_sensors_generate_aocs(float t)
{
    sim_aocs_telemetry_t d;
    float w_orbit = 0.0001f;
    float w_tumble = 0.01f;

    float cos_orbit = cosf(2.0f * M_PI * w_orbit * t);
    float sin_orbit = sinf(2.0f * M_PI * w_orbit * t);

    d.accel_x = (int16_t)(10.0f * sinf(2.0f * M_PI * w_tumble * t));
    d.accel_y = (int16_t)(10.0f * cosf(2.0f * M_PI * w_tumble * t * 0.7f));
    d.accel_z = (int16_t)(1000.0f + 5.0f * sinf(2.0f * M_PI * w_tumble * t * 0.3f));

    d.gyro_x = (int16_t)(50.0f * sinf(2.0f * M_PI * 0.001f * t));
    d.gyro_y = (int16_t)(30.0f * cosf(2.0f * M_PI * 0.0015f * t));
    d.gyro_z = (int16_t)(200.0f + 10.0f * sinf(2.0f * M_PI * 0.0005f * t));

    d.mag_x = (int16_t)(200.0f * cos_orbit + 50.0f * sinf(0.01f * t));
    d.mag_y = (int16_t)(200.0f * sin_orbit + 50.0f * cosf(0.01f * t));
    d.mag_z = (int16_t)(40000.0f + 100.0f * sinf(0.005f * t));

    d.imu_valid = 1;
    d.mag_valid = 1;
    d.timestamp_ms = (uint32_t)t;

    return d;
}

sim_cloud_data_t sim_sensors_generate_cloud(float t, uint8_t crystals_en)
{
    sim_cloud_data_t d;
    float w = 0.01f;

    d.gpio_expander = 0x00;
    d.face = 0;

    for (int i = 0; i < 16; i++) {
        float base = 2048.0f + 50.0f * sinf(2.0f * M_PI * w * t + i * 0.5f);
        float noise = 10.0f * sinf(2.0f * M_PI * 0.1f * t + i * 1.3f);
        d.channels[i] = (uint16_t)(base + noise);
    }

    if (crystals_en) {
        float crystal_signal = 800.0f + 200.0f * sinf(2.0f * M_PI * 0.003f * t);
        d.channels[4] = (uint16_t)(crystal_signal + 30.0f * sinf(2.0f * M_PI * 0.05f * t));
        d.channels[5] = (uint16_t)(crystal_signal * 0.8f + 25.0f * sinf(2.0f * M_PI * 0.04f * t));
    } else {
        d.channels[4] = 0;
        d.channels[5] = 0;
    }

    d.gpio_expander = crystals_en ? 0x80 : 0x00;

    return d;
}

sim_temp_data_t sim_sensors_generate_temps(float t)
{
    sim_temp_data_t d;
    float w = 0.001f;

    d.temps[0] = (int16_t)(200.0f + 30.0f * sinf(2.0f * M_PI * w * t));
    d.temps[1] = (int16_t)(210.0f + 25.0f * sinf(2.0f * M_PI * w * t * 0.8f + 1.0f));
    d.temps[2] = (int16_t)(195.0f + 35.0f * sinf(2.0f * M_PI * w * t * 1.2f + 2.0f));
    d.temps[3] = (int16_t)(205.0f + 20.0f * sinf(2.0f * M_PI * w * t * 0.6f + 3.0f));

    return d;
}
