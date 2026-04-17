#ifndef SIM_PROTOCOL_H
#define SIM_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Simulation protocol for RedPill (J2050) PocketQube HIL verification.
 *
 * Per RED_DES_ElectronicArchitecture_V1:
 *   - Subsystem SPI bus: OBC (master) ↔ AOCS (slave), EPS (slave), LoRa
 *   - Payload SPI bus:  OBC (master) ↔ CLOUD electronics, ArduCam camera
 *   - OBC I2C_EXT:       OBC ↔ MCP23017 GPIO expander (SPI CS control)
 *   - OBC I2C_CAM:       OBC ↔ ArduCam register config
 *   - AOCS I2C (local):  AOCS MCU ↔ ASM330LHH IMU, IIS2MDC MAG (x2 redundant)
 *   - EPS I2C (local):   EPS MCU ↔ BQ76905 battery monitor
 *   - USART:             Debug/telemetry via umbilical connector
 *   - 1-Wire:            4x TMP1827 temperature sensors on OBC PCB
 *   - INT1, INT2:        Asynchronous event lines from subsystems to OBC
 *   - NRST:              System reset line
 *   - POK:               Power OK per board
 *
 * This protocol simulates all of the above between two ESP32s.
 * The ESP32-Simulator acts as AOCS slave, EPS slave, LoRa ground station,
 * CLOUD sensor electronics, camera, GPIO expander, and TMP1827 sensors.
 * The ESP32-OBC runs the JOS OBSW and queries them via UART/SPI.
 */

#define SIM_MAGIC          0x53494D48  /* "SIMH" */
#define SIM_VERSION        2

#define SIM_SPI_CLK_HZ     1000000    /* 1 MHz */

/* ---- Transport commands (UART between ESP32s) ---- */
#define SIM_CMD_NOP                    0x00
#define SIM_CMD_PING                   0x01
#define SIM_CMD_PONG                   0x02

/* ---- Subsystem SPI bus commands (OBC ↔ AOCS, EPS, LoRa) ---- */
#define SIM_CMD_SUBSYS_QUERY_AOCS       0x10
#define SIM_CMD_SUBSYS_QUERY_EPS        0x11
#define SIM_CMD_SUBSYS_QUERY_LORA       0x12
#define SIM_CMD_SUBSYS_EPS_POWER_CMD    0x13
#define SIM_CMD_SUBSYS_AOCS_CTRL        0x14

/* ---- Payload SPI bus commands (OBC ↔ CLOUD, Camera) ---- */
#define SIM_CMD_PAYLOAD_QUERY_CLOUD     0x20
#define SIM_CMD_PAYLOAD_QUERY_CAMERA    0x21
#define SIM_CMD_PAYLOAD_CLOUD_ADC       0x22

/* ---- LoRa simulation (ground station) ---- */
#define SIM_CMD_LORA_TX_BEACON          0x30
#define SIM_CMD_LORA_RX_COMMAND         0x31
#define SIM_CMD_LORA_TX_DATA            0x32

/* ---- GPIO and signals ---- */
#define SIM_CMD_NOTIFY_GPIO             0x40
#define SIM_CMD_GET_GPIO_STATE          0x41
#define SIM_CMD_ASSERT_INT1             0x42
#define SIM_CMD_ASSERT_INT2             0x43
#define SIM_CMD_ASSERT_NRST             0x44
#define SIM_CMD_GET_POK                 0x45

/* ---- I2C simulation ---- */
#define SIM_CMD_I2C_GPIO_EXPANDER_RD    0x50
#define SIM_CMD_I2C_GPIO_EXPANDER_WR    0x51
#define SIM_CMD_I2C_CAM_RD              0x52

/* ---- 1-Wire temperature sensors ---- */
#define SIM_CMD_GET_TEMPS               0x60

/* ---- Simulation state ---- */
#define SIM_CMD_SET_SIM_STATE           0x70
#define SIM_CMD_GET_SIM_STATE           0x71
#define SIM_CMD_TELEMETRY_SNAPSHOT      0x72

/* ---- Responses ---- */
#define SIM_RESP_ACK                   0x80
#define SIM_RESP_NAK                   0x81
#define SIM_RESP_DATA                  0x82

/* ---- OBC pogo-pin mapping (from architecture doc) ---- */
#define OBC_PIN_INT1           1
#define OBC_PIN_INT2           2
#define OBC_PIN_RESERVED       3
#define OBC_PIN_TEMP_GPIO      4
#define OBC_PIN_POK_OBC        5
#define OBC_PIN_I2C_EXT_SCL    6
#define OBC_PIN_I2C_EXT_SDA    7
#define OBC_PIN_I2C_CAM_SCL    8
#define OBC_PIN_I2C_CAM_SDA    9
#define OBC_PIN_SPI_PAY_MOSI   10
#define OBC_PIN_SPI_PAY_MISO   11
#define OBC_PIN_SPI_PAY_CLK    12
#define OBC_PIN_SPI_BUS_MOSI   13
#define OBC_PIN_SPI_BUS_MISO   14
#define OBC_PIN_SPI_BUS_CLK    15
#define OBC_PIN_NRST           16
#define OBC_PIN_USART_TX       17
#define OBC_PIN_USART_RX       18
#define OBC_PIN_VBAT           19
#define OBC_PIN_GND            20

/* ---- SPI chip-select targets (controlled by MCP23017 GPIO expander) ---- */
#define SPI_CS_AOCS           0
#define SPI_CS_EPS            1
#define SPI_CS_LORA           2
#define SPI_CS_FRAM_0         3
#define SPI_CS_FRAM_1         4
#define SPI_CS_FRAM_2         5
#define SPI_CS_FRAM_3         6
#define SPI_CS_CLOUD_ADC      7
#define SPI_CS_CAMERA         8

#pragma pack(push, 1)

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  cmd;
    uint8_t  len;
    uint8_t  seq;
} sim_header_t;

/* ---- EPS telemetry (from BQ76905 via EPS MCU over subsystem SPI) ---- */
typedef struct {
    uint8_t  soc;            /* State of Charge 0-100% */
    uint8_t  soh;            /* State of Health 0-100% */
    int16_t  temp_c;         /* Battery temperature (0.1 C units) */
    uint16_t voltage_mv;     /* Battery bus voltage (mV), 2S Li-ion: 6000-8400 */
    int16_t  current_ma;     /* Current (mA) */
    uint16_t flags;          /* Fault flags from BQ76905 */
} sim_eps_telemetry_t;

/* ---- AOCS telemetry (from IMU+MAG via AOCS MCU over subsystem SPI) ---- */
typedef struct {
    int16_t  accel_x;
    int16_t  accel_y;
    int16_t  accel_z;
    int16_t  gyro_x;
    int16_t  gyro_y;
    int16_t  gyro_z;
    int16_t  mag_x;
    int16_t  mag_y;
    int16_t  mag_z;
    uint8_t  imu_valid;
    uint8_t  mag_valid;
    uint32_t timestamp_ms;
} sim_aocs_telemetry_t;

/* ---- CLOUD sensor data (MAX11228 ADC via payload SPI) ---- */
typedef struct {
    uint16_t channels[16];  /* 16 ADC channels for one face */
    uint8_t  gpio_expander; /* MCP23017 register snapshot */
    uint8_t  face;
} sim_cloud_data_t;

/* ---- CLEAR/Crystals GPIO state (via Conversion Z+ board) ---- */
typedef struct {
    uint8_t  crystals_en;
    uint8_t  crystals_heater;
    uint8_t  crystals_cam_trig;
    uint8_t  clear_led_fz_a;
    uint8_t  clear_led_fz_b;
    uint8_t  clear_led_fmz;
    uint8_t  _pad;
} sim_payload_gpio_t;

/* ---- Temperature sensors (4x TMP1827 on OBC PCB, 1-Wire) ---- */
typedef struct {
    int16_t  temps[4];       /* PCB temperatures in 0.1 C units */
} sim_temp_data_t;

/* ---- Power distribution state ---- */
typedef struct {
    uint8_t  pok_obc;        /* OBC Power OK */
    uint8_t  pok_aocs;       /* AOCS Power OK */
    uint8_t  pok_eps;        /* EPS Power OK (always on) */
    uint8_t  pok_telecomm;   /* LoRa PCB Power OK */
    uint8_t  pok_conv_zp;    /* Conversion Z+ Power OK */
} sim_pok_state_t;

/* ---- GPIO expander state (MCP23017) ---- */
typedef struct {
    uint8_t  outputs;        /* GPIOA output register */
    uint8_t  directions;     /* IODIRA direction register */
    uint8_t  inputs;         /* GPIOB input register (read back) */
} sim_gpio_expander_t;

/* ---- LoRa telecommand (from simulated ground station) ---- */
typedef struct {
    uint8_t  cmd_id;
    uint8_t  payload[63];
    uint8_t  payload_len;
} sim_lora_cmd_t;

/* ---- Full telemetry snapshot ---- */
typedef struct {
    uint32_t timestamp_ms;
    uint8_t  state;
    uint8_t  eps_soc;
    uint8_t  trigger;
    uint8_t  flags;
    uint16_t voltage_mv;
    int16_t  temp_c;
    uint8_t  pok_mask;
    uint8_t  beacon_interval_s;
    uint8_t  _pad[20];
} sim_telemetry_t;

#pragma pack(pop)

#define SIM_MAX_PAYLOAD 56

typedef struct {
    sim_header_t hdr;
    uint8_t payload[SIM_MAX_PAYLOAD];
} sim_packet_t;

#define SIM_PKT_SIZE(pkt) (sizeof(sim_header_t) + (pkt)->hdr.len)

#ifdef __cplusplus
}
#endif

#endif /* SIM_PROTOCOL_H */
