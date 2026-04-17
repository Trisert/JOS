#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/i2c.h"

#include "sim_protocol.h"
#include "sim_sensors.h"
#include "sim_scenarios.h"

static const char *TAG = "JOS-SIM";

#define OBC_SPI_HOST    SPI2_HOST
#define OBC_SPI_MISO    GPIO_NUM_19
#define OBC_SPI_MOSI    GPIO_NUM_23
#define OBC_SPI_SCLK    GPIO_NUM_18
#define OBC_SPI_CS      GPIO_NUM_5

#define CONSOLE_UART    UART_NUM_0
#define CONSOLE_UART_BAUD 115200

#define OBC_TX_UART     UART_NUM_1
#define OBC_TX_PIN      GPIO_NUM_17
#define OBC_RX_PIN      GPIO_NUM_16
#define OBC_UART_BAUD   115200

static QueueHandle_t console_rx_queue;
static SemaphoreHandle_t sim_state_mutex;

static sim_state_t sim = {
    .bms = {
        .soc = 100,
        .temp_c = 250,
        .voltage_mv = 4200,
    },
    .imu = {
        .accel_x = 0, .accel_y = 0, .accel_z = 1000,
        .gyro_x = 0, .gyro_y = 0, .gyro_z = 0,
    },
    .mag = {
        .mag_x = 200, .mag_y = 0, .mag_z = 40000,
    },
    .adc = {
        .ch4_mv = 1500,
        .ch5_mv = 1200,
        .ch8_mv = 800,
        .ch9_mv = 750,
    },
    .cloud_adc = {0},
    .running = false,
    .time_multiplier = 1,
    .auto_mode = false,
};

static sim_scenario_t *current_scenario = NULL;

static void print_help(void)
{
    printf("\n=== JOS Satellite Simulator ===\n");
    printf("Commands:\n");
    printf("  help              - Show this help\n");
    printf("  start             - Start simulation\n");
    printf("  stop              - Stop simulation\n");
    printf("  status            - Show current state\n");
    printf("  reset             - Reset all to defaults\n");
    printf("  bms <soc> <temp> <voltage>  - Set battery (e.g. bms 80 250 4100)\n");
    printf("  drain <rate>      - Drain battery over time (%%/min)\n");
    printf("  charge <rate>     - Charge battery over time (%%/min)\n");
    printf("  imu <ax> <ay> <az> <gx> <gy> <gz>  - Set IMU (raw 16-bit)\n");
    printf("  mag <x> <y> <z>  - Set magnetometer (raw 16-bit)\n");
    printf("  adc <ch4> <ch5> <ch8> <ch9>  - Set ADC channels (mV)\n");
    printf("  cmd <id> [hex_payload]  - Send telecommand to OBC\n");
    printf("  activate          - Send ACTIVATE_PAYLOAD cmd\n");
    printf("  ready             - Send EXIT_STATE cmd\n");
    printf("  reset_obc         - Send RESET cmd\n");
    printf("  scenario <name>   - Run preset scenario\n");
    printf("  list              - List available scenarios\n");
    printf("  speed <mult>      - Set time multiplier\n");
    printf("  auto              - Toggle auto scenario mode\n");
    printf("\n");
}

static void print_status(void)
{
    xSemaphoreTake(sim_state_mutex, portMAX_DELAY);

    printf("\n--- Simulation Status ---\n");
    printf("  Running:      %s\n", sim.running ? "YES" : "NO");
    printf("  Time Mult:    %dx\n", sim.time_multiplier);
    printf("  Auto Mode:    %s\n", sim.auto_mode ? "ON" : "OFF");
    printf("  Scenario:     %s\n", current_scenario ? current_scenario->name : "None");

    printf("  BMS:\n");
    printf("    SoC:        %d%%\n", sim.bms.soc);
    printf("    Temp:       %d.%d C\n", sim.bms.temp_c / 10, abs(sim.bms.temp_c % 10));
    printf("    Voltage:    %dmV\n", sim.bms.voltage_mv);

    printf("  IMU:\n");
    printf("    Accel:      %d %d %d\n", sim.imu.accel_x, sim.imu.accel_y, sim.imu.accel_z);
    printf("    Gyro:       %d %d %d\n", sim.imu.gyro_x, sim.imu.gyro_y, sim.imu.gyro_z);

    printf("  Mag:\n");
    printf("    Field:      %d %d %d\n", sim.mag.mag_x, sim.mag.mag_y, sim.mag.mag_z);

    printf("  ADC (mV):\n");
    printf("    CH4(Front): %d  CH5(Rear): %d\n", sim.adc.ch4_mv, sim.adc.ch5_mv);
    printf("    CH8(+Z):    %d  CH9(-Z):  %d\n", sim.adc.ch8_mv, sim.adc.ch9_mv);

    xSemaphoreGive(sim_state_mutex);
    printf("-------------------------\n\n");
}

static void send_to_obc(const uint8_t *data, size_t len)
{
    uart_write_bytes(OBC_TX_UART, data, len);
}

static void send_bms_to_obc(void)
{
    sim_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.hdr.magic = SIM_MAGIC;
    pkt.hdr.version = SIM_VERSION;
    pkt.hdr.cmd = SIM_RESP_DATA;
    pkt.hdr.len = sizeof(sim_bms_data_t);

    memcpy(pkt.payload, &sim.bms, sizeof(sim_bms_data_t));
    send_to_obc((uint8_t *)&pkt, SIM_PKT_SIZE(&pkt));
}

static void send_adc_to_obc(void)
{
    sim_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.hdr.magic = SIM_MAGIC;
    pkt.hdr.version = SIM_VERSION;
    pkt.hdr.cmd = SIM_RESP_DATA + 1;
    pkt.hdr.len = sizeof(sim_adc_data_t);

    memcpy(pkt.payload, &sim.adc, sizeof(sim_adc_data_t));
    send_to_obc((uint8_t *)&pkt, SIM_PKT_SIZE(&pkt));
}

static void send_telecommand(uint8_t cmd_id, const uint8_t *payload, uint8_t payload_len)
{
    sim_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.hdr.magic = SIM_MAGIC;
    pkt.hdr.version = SIM_VERSION;
    pkt.hdr.cmd = SIM_LORA_RX_COMMAND;
    pkt.hdr.len = 1 + payload_len;

    pkt.payload[0] = cmd_id;
    if (payload && payload_len > 0) {
        memcpy(pkt.payload + 1, payload, payload_len);
    }

    send_to_obc((uint8_t *)&pkt, SIM_PKT_SIZE(&pkt));
    ESP_LOGI(TAG, "Telecommand 0x%02x sent to OBC", cmd_id);
}

static void cmd_bms(int argc, char **argv)
{
    if (argc < 4) {
        printf("Usage: bms <soc> <temp_x10> <voltage_mv>\n");
        return;
    }
    xSemaphoreTake(sim_state_mutex, portMAX_DELAY);
    sim.bms.soc = (uint8_t)atoi(argv[1]);
    sim.bms.temp_c = (int16_t)atoi(argv[2]);
    sim.bms.voltage_mv = (uint16_t)atoi(argv[3]);
    xSemaphoreGive(sim_state_mutex);

    send_bms_to_obc();
    printf("BMS updated: SoC=%d%% T=%d.%dC V=%dmV\n",
           sim.bms.soc, sim.bms.temp_c / 10, abs(sim.bms.temp_c % 10), sim.bms.voltage_mv);
}

static void cmd_imu(int argc, char **argv)
{
    if (argc < 7) {
        printf("Usage: imu <ax> <ay> <az> <gx> <gy> <gz>\n");
        return;
    }
    xSemaphoreTake(sim_state_mutex, portMAX_DELAY);
    sim.imu.accel_x = (int16_t)atoi(argv[1]);
    sim.imu.accel_y = (int16_t)atoi(argv[2]);
    sim.imu.accel_z = (int16_t)atoi(argv[3]);
    sim.imu.gyro_x = (int16_t)atoi(argv[4]);
    sim.imu.gyro_y = (int16_t)atoi(argv[5]);
    sim.imu.gyro_z = (int16_t)atoi(argv[6]);
    sim.imu.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    xSemaphoreGive(sim_state_mutex);
    printf("IMU updated\n");
}

static void cmd_mag(int argc, char **argv)
{
    if (argc < 4) {
        printf("Usage: mag <x> <y> <z>\n");
        return;
    }
    xSemaphoreTake(sim_state_mutex, portMAX_DELAY);
    sim.mag.mag_x = (int16_t)atoi(argv[1]);
    sim.mag.mag_y = (int16_t)atoi(argv[2]);
    sim.mag.mag_z = (int16_t)atoi(argv[3]);
    sim.mag.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    xSemaphoreGive(sim_state_mutex);
    printf("Magnetometer updated\n");
}

static void cmd_adc(int argc, char **argv)
{
    if (argc < 5) {
        printf("Usage: adc <ch4> <ch5> <ch8> <ch9>\n");
        return;
    }
    xSemaphoreTake(sim_state_mutex, portMAX_DELAY);
    sim.adc.ch4_mv = (uint16_t)atoi(argv[1]);
    sim.adc.ch5_mv = (uint16_t)atoi(argv[2]);
    sim.adc.ch8_mv = (uint16_t)atoi(argv[3]);
    sim.adc.ch9_mv = (uint16_t)atoi(argv[4]);
    xSemaphoreGive(sim_state_mutex);

    send_adc_to_obc();
    printf("ADC updated: CH4=%d CH5=%d CH8=%d CH9=%d mV\n",
           sim.adc.ch4_mv, sim.adc.ch5_mv, sim.adc.ch8_mv, sim.adc.ch9_mv);
}

static void cmd_telecommand(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: cmd <id> [hex_payload]\n");
        return;
    }
    uint8_t cmd_id = (uint8_t)strtol(argv[1], NULL, 0);
    uint8_t payload[63];
    uint8_t payload_len = 0;

    if (argc >= 3) {
        const char *hex = argv[2];
        while (*hex && payload_len < 63) {
            while (*hex == ' ' || *hex == ',') hex++;
            if (!*hex) break;
            char *end;
            payload[payload_len++] = (uint8_t)strtol(hex, &end, 16);
            hex = end;
        }
    }

    send_telecommand(cmd_id, payload, payload_len);
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t max_len)
{
    size_t len = 0;
    while (*hex && len < max_len) {
        while (*hex == ' ') hex++;
        if (!*hex) break;
        out[len++] = (uint8_t)strtol(hex, NULL, 16);
        while (*hex && *hex != ' ') hex++;
    }
    return (int)len;
}

static void cmd_drain(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: drain <percent_per_minute>\n");
        return;
    }
    sim.drain_rate = (float)atof(argv[1]);
    printf("Battery drain rate: %.1f %%/min\n", sim.drain_rate);
}

static void cmd_charge(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: charge <percent_per_minute>\n");
        return;
    }
    sim.drain_rate = -(float)atof(argv[1]);
    printf("Battery charge rate: %.1f %%/min\n", -sim.drain_rate);
}

static void cmd_scenario(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: scenario <name>\n");
        return;
    }
    sim_scenario_t *sc = sim_scenario_find(argv[1]);
    if (!sc) {
        printf("Scenario '%s' not found. Use 'list' to see available.\n", argv[1]);
        return;
    }
    current_scenario = sc;
    sc->reset();
    sc->start();
    printf("Scenario '%s' started.\n", sc->name);
}

static void cmd_list(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("\nAvailable scenarios:\n");
    int i = 0;
    while (sim_scenario_list[i]) {
        printf("  %-20s - %s\n", sim_scenario_list[i]->name, sim_scenario_list[i]->desc);
        i++;
    }
    printf("\n");
}

static void cmd_speed(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: speed <multiplier>\n");
        return;
    }
    sim.time_multiplier = atoi(argv[1]);
    if (sim.time_multiplier < 1) sim.time_multiplier = 1;
    if (sim.time_multiplier > 1000) sim.time_multiplier = 1000;
    printf("Time multiplier set to %dx\n", sim.time_multiplier);
}

typedef struct {
    const char *name;
    void (*func)(int argc, char **argv);
} console_cmd_t;

static const console_cmd_t commands[] = {
    {"help",     NULL},
    {"start",    NULL},
    {"stop",     NULL},
    {"status",   NULL},
    {"reset",    NULL},
    {"bms",      cmd_bms},
    {"drain",    cmd_drain},
    {"charge",   cmd_charge},
    {"imu",      cmd_imu},
    {"mag",      cmd_mag},
    {"adc",      cmd_adc},
    {"cmd",      cmd_telecommand},
    {"activate", NULL},
    {"ready",    NULL},
    {"reset_obc",NULL},
    {"scenario", cmd_scenario},
    {"list",     cmd_list},
    {"speed",    cmd_speed},
    {"auto",     NULL},
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

static void console_task(void *arg)
{
    (void)arg;
    char line[256];
    char *argv[16];
    int argc;

    printf("\n\n========================================\n");
    printf("  JOS Satellite Simulator v%d.%d\n", SIM_VERSION, 0);
    printf("  ESP32-SIM | RedPill CubeSat\n");
    printf("========================================\n");
    print_help();

    while (1) {
        printf("> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        argc = 0;
        char *tok = strtok(line, " ");
        while (tok && argc < 15) {
            argv[argc++] = tok;
            tok = strtok(NULL, " ");
        }

        if (argc == 0) continue;

        int found = 0;
        for (size_t i = 0; i < NUM_COMMANDS; i++) {
            if (strcasecmp(argv[0], commands[i].name) == 0) {
                found = 1;
                switch (i) {
                case 0: print_help(); break;
                case 1:
                    sim.running = true;
                    printf("Simulation started\n");
                    break;
                case 2:
                    sim.running = false;
                    printf("Simulation stopped\n");
                    break;
                case 3: print_status(); break;
                case 4:
                    xSemaphoreTake(sim_state_mutex, portMAX_DELAY);
                    sim.bms.soc = 100;
                    sim.bms.temp_c = 250;
                    sim.bms.voltage_mv = 4200;
                    sim.drain_rate = 0;
                    xSemaphoreGive(sim_state_mutex);
                    send_bms_to_obc();
                    printf("Reset to defaults\n");
                    break;
                case 12:
                    send_telecommand(0x05, NULL, 0);
                    break;
                case 13:
                    send_telecommand(0x02, NULL, 0);
                    break;
                case 14:
                    send_telecommand(0x01, NULL, 0);
                    break;
                case 18:
                    sim.auto_mode = !sim.auto_mode;
                    printf("Auto mode: %s\n", sim.auto_mode ? "ON" : "OFF");
                    break;
                default:
                    if (commands[i].func) {
                        commands[i].func(argc, argv);
                    }
                    break;
                }
                break;
            }
        }
        if (!found) {
            printf("Unknown command: '%s'. Type 'help' for commands.\n", argv[0]);
        }
    }
}

static void sim_update_task(void *arg)
{
    (void)arg;
    uint32_t last_bms_ms = 0;
    uint32_t last_adc_ms = 0;
    uint32_t last_imu_ms = 0;
    uint32_t last_scenario_ms = 0;

    while (1) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t dt = now - last_bms_ms;

        if (sim.running) {
            xSemaphoreTake(sim_state_mutex, portMAX_DELAY);

            if (sim.drain_rate != 0 && dt >= 1000) {
                int32_t delta = (int32_t)(sim.drain_rate * dt / 60000.0f * sim.time_multiplier);
                int32_t new_soc = (int32_t)sim.bms.soc + delta;
                if (new_soc < 0) new_soc = 0;
                if (new_soc > 100) new_soc = 100;
                sim.bms.soc = (uint8_t)new_soc;

                sim.bms.voltage_mv = (uint16_t)(3000 + (sim.bms.soc * 12));
                if (sim.bms.soc > 80) {
                    sim.bms.temp_c = 250;
                } else if (sim.bms.soc > 40) {
                    sim.bms.temp_c = 200;
                } else {
                    sim.bms.temp_c = 150;
                }

                last_bms_ms = now;
                send_bms_to_obc();
            }

            if (sim.auto_mode && (now - last_scenario_ms) >= 1000) {
                for (int i = 0; sim_scenario_list[i]; i++) {
                    sim_scenario_list[i]->step((now - last_scenario_ms) * sim.time_multiplier);
                }
                last_scenario_ms = now;
            }

            if (current_scenario && (now - last_scenario_ms) >= 100) {
                current_scenario->step((now - last_scenario_ms) * sim.time_multiplier);
                last_scenario_ms = now;
            }

            xSemaphoreGive(sim_state_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void spi_slave_task(void *arg)
{
    (void)arg;

    spi_slave_transaction_t slv_trans;
    uint8_t rx_buf[64];
    uint8_t tx_buf[64];

    memset(&slv_trans, 0, sizeof(slv_trans));
    slv_trans.length = 64 * 8;
    slv_trans.rx_buffer = rx_buf;
    slv_trans.tx_buffer = tx_buf;

    spi_slave_transaction_t *ret_trans;

    while (1) {
        memset(tx_buf, 0, sizeof(tx_buf));
        slv_trans.length = 64 * 8;
        slv_trans.trans_len = 0;

        esp_err_t rc = spi_slave_transmit(SPI2_HOST, &slv_trans, portMAX_DELAY);
        if (rc != ESP_OK) continue;

        if (slv_trans.trans_len < 8) continue;

        sim_header_t *hdr = (sim_header_t *)rx_buf;
        if (hdr->magic != SIM_MAGIC) continue;

        switch (hdr->cmd) {
        case SIM_CMD_GET_BMS:
            memset(tx_buf, 0, sizeof(tx_buf));
            memcpy(tx_buf, &sim.bms, sizeof(sim_bms_data_t));
            ESP_LOGI(TAG, "SPI: BMS request from OBC");
            break;

        case SIM_CMD_GET_ADC_CHANNELS:
            memset(tx_buf, 0, sizeof(tx_buf));
            memcpy(tx_buf, &sim.adc, sizeof(sim_adc_data_t));
            break;

        case SIM_CMD_NOTIFY_GPIO:
            ESP_LOGI(TAG, "SPI: GPIO state notification from OBC");
            if (hdr->len >= sizeof(sim_gpio_state_t)) {
                sim_gpio_state_t *gpio = (sim_gpio_state_t *)(rx_buf + sizeof(sim_header_t));
                ESP_LOGI(TAG, "  CRYSTALS: en=%d heater=%d cam=%d",
                         gpio->crystals_en, gpio->crystals_heater, gpio->crystals_cam_trig);
                ESP_LOGI(TAG, "  CLEAR: +Z_A=%d +Z_B=%d -Z=%d",
                         gpio->clear_led_fz_a, gpio->clear_led_fz_b, gpio->clear_led_fmz);
            }
            break;

        default:
            break;
        }
    }
}

static void obc_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];

    while (1) {
        int len = uart_read_bytes(OBC_TX_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len > 0) {
            sim_header_t *hdr = (sim_header_t *)buf;
            if (hdr->magic == SIM_MAGIC) {
                switch (hdr->cmd) {
                case SIM_CMD_LORA_TX_BEACON:
                    ESP_LOGI(TAG, "Beacon received from OBC (%d bytes)", len);
                    break;
                case SIM_CMD_LORA_TX_DATA:
                    ESP_LOGI(TAG, "Data received from OBC (%d bytes)", len);
                    break;
                default:
                    break;
                }
            }
        }
    }
}

static void spi_slave_init(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = OBC_SPI_MOSI,
        .miso_io_num = OBC_SPI_MISO,
        .sclk_io_num = OBC_SPI_SCLK,
    };

    spi_slave_device_config_t slv_cfg = {
        .spics_io_num = OBC_SPI_CS,
        .flags = 0,
        .queue_size = 2,
        .mode = 0,
    };

    ESP_ERROR_CHECK(spi_slave_initialize(SPI2_HOST, &bus_cfg, &slv_cfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "SPI slave initialized (CS=GPIO%d)", OBC_SPI_CS);
}

static void uart_to_obc_init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate = OBC_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_param_config(OBC_TX_UART, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(OBC_TX_UART, OBC_TX_PIN, OBC_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(OBC_TX_UART, 256, 256, 0, NULL, 0));
    ESP_LOGI(TAG, "OBC UART initialized (TX=GPIO%d RX=GPIO%d)", OBC_TX_PIN, OBC_RX_PIN);
}

void app_main(void)
{
    printf("\n\n");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  JOS Satellite Simulator");
    ESP_LOGI(TAG, "  ESP32-SIM Hardware-in-the-Loop");
    ESP_LOGI(TAG, "========================================");

    sim_state_mutex = xSemaphoreCreateMutex();

    sim_sensors_init(&sim);
    sim_scenarios_init(&sim);

    spi_slave_init();
    uart_to_obc_init();

    xTaskCreate(console_task, "console", 8192, NULL, 2, NULL);
    xTaskCreate(sim_update_task, "simUpdate", 4096, NULL, 3, NULL);
    xTaskCreate(spi_slave_task, "spiSlave", 4096, NULL, 4, NULL);
    xTaskCreate(obc_rx_task, "obcRx", 4096, NULL, 2, NULL);

    ESP_LOGI(TAG, "All simulator tasks running. Type 'help' for commands.");
}
