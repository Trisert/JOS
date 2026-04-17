#ifndef SIM_PROTOCOL_H
#define SIM_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_MAGIC          0x53494D48  /* "SIMH" */
#define SIM_VERSION        1

#define SIM_SPI_CLK_HZ     1000000    /* 1 MHz SPI between ESP32s */

#define SIM_CMD_NOP                    0x00
#define SIM_CMD_PING                   0x01
#define SIM_CMD_PONG                   0x02
#define SIM_CMD_SET_BMS                0x10
#define SIM_CMD_GET_BMS                0x11
#define SIM_CMD_SET_IMU_DATA           0x20
#define SIM_CMD_GET_IMU_DATA           0x21
#define SIM_CMD_SET_MAG_DATA           0x22
#define SIM_CMD_GET_MAG_DATA           0x23
#define SIM_CMD_SET_ADC_CHANNELS       0x30
#define SIM_CMD_GET_ADC_CHANNELS       0x31
#define SIM_CMD_SET_CLOUD_ADC          0x32
#define SIM_CMD_GET_CLOUD_ADC          0x33
#define SIM_CMD_LORA_TX_BEACON         0x40
#define SIM_CMD_LORA_RX_COMMAND        0x41
#define SIM_CMD_LORA_TX_DATA           0x42
#define SIM_CMD_GET_GPIO_STATE         0x50
#define SIM_CMD_NOTIFY_GPIO            0x51
#define SIM_CMD_SET_SIM_STATE          0x60
#define SIM_CMD_GET_SIM_STATE          0x61
#define SIM_CMD_TELEMETRY_SNAPSHOT     0x70

#define SIM_RESP_ACK                   0x80
#define SIM_RESP_NAK                   0x81
#define SIM_RESP_DATA                  0x82

#pragma pack(push, 1)

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  cmd;
    uint8_t  len;
    uint8_t  seq;
} sim_header_t;

typedef struct {
    uint8_t  soc;
    int16_t  temp_c;
    uint16_t voltage_mv;
} sim_bms_data_t;

typedef struct {
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
    uint32_t timestamp_ms;
} sim_imu_data_t;

typedef struct {
    int16_t mag_x;
    int16_t mag_y;
    int16_t mag_z;
    uint32_t timestamp_ms;
} sim_mag_data_t;

typedef struct {
    uint16_t ch4_mv;
    uint16_t ch5_mv;
    uint16_t ch8_mv;
    uint16_t ch9_mv;
} sim_adc_data_t;

typedef struct {
    uint16_t channels[16];
} sim_cloud_adc_t;

typedef struct {
    uint32_t timestamp_ms;
    uint8_t  state;
    uint8_t  bms_soc;
    uint8_t  trigger;
    uint8_t  flags;
    uint16_t voltage_mv;
    int16_t  temp_c;
    uint8_t  beacon_interval_s;
    uint8_t  _pad[21];
} sim_telemetry_t;

typedef struct {
    uint8_t  crystals_en;
    uint8_t  crystals_heater;
    uint8_t  crystals_cam_trig;
    uint8_t  clear_led_fz_a;
    uint8_t  clear_led_fz_b;
    uint8_t  clear_led_fmz;
    uint8_t  _pad[1];
} sim_gpio_state_t;

typedef struct {
    uint8_t  cmd_id;
    uint8_t  payload[63];
    uint8_t  payload_len;
} sim_lora_cmd_t;

#pragma pack(pop)

#define SIM_MAX_PAYLOAD 56

typedef struct {
    sim_header_t hdr;
    uint8_t payload[SIM_MAX_PAYLOAD];
} sim_packet_t;

static inline uint8_t sim_calc_crc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
    }
    return crc;
}

#define SIM_PKT_SIZE(pkt) (sizeof(sim_header_t) + (pkt)->hdr.len)

#ifdef __cplusplus
}
#endif

#endif /* SIM_PROTOCOL_H */
