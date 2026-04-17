#include "sim_sensors.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static sim_state_t *s_state;

void sim_sensors_init(sim_state_t *state)
{
    s_state = state;
    s_state->boot_tick_ms = 0;

    for (int i = 0; i < 16; i++) {
        s_state->cloud_adc.channels[i] = 2048;
    }
}

sim_imu_data_t sim_sensors_generate_imu(float t)
{
    sim_imu_data_t d;
    float w = 0.01f;

    d.accel_x = (int16_t)(10.0f * sinf(2.0f * M_PI * w * t));
    d.accel_y = (int16_t)(10.0f * cosf(2.0f * M_PI * w * t * 0.7f));
    d.accel_z = (int16_t)(1000.0f + 5.0f * sinf(2.0f * M_PI * w * t * 0.3f));

    d.gyro_x = (int16_t)(50.0f * sinf(2.0f * M_PI * 0.001f * t));
    d.gyro_y = (int16_t)(30.0f * cosf(2.0f * M_PI * 0.0015f * t));
    d.gyro_z = (int16_t)(200.0f + 10.0f * sinf(2.0f * M_PI * 0.0005f * t));

    d.timestamp_ms = (uint32_t)t;

    return d;
}

sim_mag_data_t sim_sensors_generate_mag(float t)
{
    sim_mag_data_t d;
    float w = 0.0001f;

    float cos_t = cosf(w * t);
    float sin_t = sinf(w * t);

    d.mag_x = (int16_t)(200.0f * cos_t + 50.0f * sinf(0.01f * t));
    d.mag_y = (int16_t)(200.0f * sin_t + 50.0f * cosf(0.01f * t));
    d.mag_z = (int16_t)(40000.0f + 100.0f * sinf(0.005f * t));

    d.timestamp_ms = (uint32_t)t;

    return d;
}

sim_adc_data_t sim_sensors_generate_adc(float t, uint8_t crystals_en)
{
    sim_adc_data_t d;
    (void)t;

    if (crystals_en) {
        float noise = 20.0f * sinf(0.1f * t);
        d.ch4_mv = (uint16_t)(1500.0f + noise);
        d.ch5_mv = (uint16_t)(1200.0f + noise * 0.8f);
    } else {
        d.ch4_mv = 0;
        d.ch5_mv = 0;
    }

    float led_noise = 10.0f * sinf(0.05f * t);
    d.ch8_mv = (uint16_t)(800.0f + led_noise);
    d.ch9_mv = (uint16_t)(750.0f + led_noise * 0.9f);

    return d;
}

sim_cloud_adc_t sim_sensors_generate_cloud_adc(float t)
{
    sim_cloud_adc_t d;
    (void)t;

    for (int i = 0; i < 16; i++) {
        d.channels[i] = 2048 + (int16_t)(50.0f * sinf(0.01f * t + i * 0.5f));
    }

    return d;
}
