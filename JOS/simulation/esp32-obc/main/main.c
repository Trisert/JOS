#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include "main.h"
#include "cmsis_os.h"
#include "state_machine.h"
#include "watchdog.h"
#include "bms.h"
#include "memory.h"
#include "comms.h"
#include "sim_protocol.h"
#include "sim_bridge.h"

static const char *TAG = "JOS-OBC";

#define SIM_SPI_HOST    SPI2_HOST
#define SIM_SPI_MISO    GPIO_NUM_19
#define SIM_SPI_MOSI    GPIO_NUM_23
#define SIM_SPI_SCLK    GPIO_NUM_18
#define SIM_SPI_CS      GPIO_NUM_5

#define SIM_UART_NUM    UART_NUM_1
#define SIM_UART_TX     GPIO_NUM_17
#define SIM_UART_RX     GPIO_NUM_16
#define SIM_UART_BAUD   115200

static spi_device_handle_t sim_spi_dev;
static QueueHandle_t sim_cmd_queue;

static SemaphoreHandle_t sim_tx_mutex;

int sim_bridge_send(const uint8_t *data, size_t len)
{
    if (!sim_spi_dev || !data || len == 0) return -1;

    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;
    t.rx_buffer = NULL;

    xSemaphoreTake(sim_tx_mutex, portMAX_DELAY);
    esp_err_t rc = spi_device_polling_transmit(sim_spi_dev, &t);
    xSemaphoreGive(sim_tx_mutex);

    return (rc == ESP_OK) ? 0 : -1;
}

int sim_bridge_send_recv(const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (!sim_spi_dev || !tx || !rx || len == 0) return -1;

    spi_transaction_t t = {0};
    t.length = len * 8;
    t.rxlength = len * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;

    xSemaphoreTake(sim_tx_mutex, portMAX_DELAY);
    esp_err_t rc = spi_device_polling_transmit(sim_spi_dev, &t);
    xSemaphoreGive(sim_tx_mutex);

    return (rc == ESP_OK) ? 0 : -1;
}

void sim_bridge_notify_gpio(const sim_gpio_state_t *gpio)
{
    if (!gpio) return;

    sim_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.hdr.magic = SIM_MAGIC;
    pkt.hdr.version = SIM_VERSION;
    pkt.hdr.cmd = SIM_CMD_NOTIFY_GPIO;
    pkt.hdr.len = sizeof(sim_gpio_state_t);

    memcpy(pkt.payload, gpio, sizeof(sim_gpio_state_t));

    sim_bridge_send((uint8_t *)&pkt, SIM_PKT_SIZE(&pkt));
}

static void sim_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];

    while (1) {
        int len = uart_read_bytes(SIM_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len > 0 && buf[0] == SIM_MAGIC) {
            if (xQueueSend(sim_cmd_queue, buf, 0) != pdTRUE) {
                ESP_LOGW(TAG, "sim cmd queue full, dropping");
            }
        }
    }
}

static void sim_process_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];

    while (1) {
        if (xQueueReceive(sim_cmd_queue, buf, portMAX_DELAY) == pdTRUE) {
            sim_header_t *hdr = (sim_header_t *)buf;

            if (hdr->magic != SIM_MAGIC) continue;
            if (hdr->len > SIM_MAX_PAYLOAD) continue;

            switch (hdr->cmd) {
            case SIM_RESP_DATA:
                if (hdr->len >= sizeof(sim_bms_data_t)) {
                    sim_bms_data_t *bms = (sim_bms_data_t *)(buf + sizeof(sim_header_t));
                    bms_set_soc_stub(bms->soc);
                    hal_compat_set_adc_value(4, 2048);
                    hal_compat_set_adc_value(5, 2048);
                    ESP_LOGI(TAG, "BMS updated: SoC=%d%% T=%d V=%dmV",
                             bms->soc, bms->temp_c, bms->voltage_mv);
                }
                break;

            case SIM_RESP_DATA + 1:
                if (hdr->len >= sizeof(sim_adc_data_t)) {
                    sim_adc_data_t *adc = (sim_adc_data_t *)(buf + sizeof(sim_header_t));
                    hal_compat_set_adc_value(4, adc->ch4_mv);
                    hal_compat_set_adc_value(5, adc->ch5_mv);
                    hal_compat_set_adc_value(8, adc->ch8_mv);
                    hal_compat_set_adc_value(9, adc->ch9_mv);
                }
                break;

            case SIM_LORA_RX_COMMAND: {
                sim_lora_cmd_t *cmd = (sim_lora_cmd_t *)(buf + sizeof(sim_header_t));
                if (hdr->len >= 1) {
                    comms_dispatch_command(cmd->cmd_id, cmd->payload, cmd->payload_len);
                    ESP_LOGI(TAG, "LoRa cmd received: 0x%02x", cmd->cmd_id);
                }
                break;
            }

            default:
                break;
            }
        }
    }
}

static void sim_spi_init(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SIM_SPI_MOSI,
        .miso_io_num = SIM_SPI_MISO,
        .sclk_io_num = SIM_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    spi_device_interface_config_t dev_cfg = {
        .mode = 0,
        .clock_speed_hz = SIM_SPI_CLK_HZ,
        .spics_io_num = SIM_SPI_CS,
        .queue_size = 4,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SIM_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SIM_SPI_HOST, &dev_cfg, &sim_spi_dev));
}

static void sim_uart_init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate = SIM_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_param_config(SIM_UART_NUM, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(SIM_UART_NUM, SIM_UART_TX, SIM_UART_RX,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(SIM_UART_NUM, 256, 256, 0, NULL, 0));
}

void StartDefaultTask(void *argument)
{
    (void)argument;
    for (;;) {
        osDelay(1);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  JOS - ESP32 OBC Verification Sim");
    ESP_LOGI(TAG, "  FYS-4 RedPill CubeSat OBSW");
    ESP_LOGI(TAG, "========================================");

    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_ADC1_Init();
    MX_I2C1_Init();
    MX_I2C2_Init();
    MX_SPI1_Init();
    MX_SPI2_Init();
    MX_TIM1_Init();

    sim_tx_mutex = xSemaphoreCreateMutex();
    sim_cmd_queue = xQueueCreate(16, 256);

    sim_spi_init();
    sim_uart_init();

    bms_init();
    fram_init();
    cyclic_buffer_init();
    laststates_init();
    lora_init();
    state_machine_init();
    watchdog_monitor_init();

    xTaskCreate(sim_rx_task, "simRX", 4096, NULL, 3, NULL);
    xTaskCreate(sim_process_task, "simProc", 4096, NULL, 3, NULL);

    osKernelInitialize();

    osThreadNew(StartDefaultTask, NULL, &(osThreadAttr_t){
        .name = "defaultTask", .stack_size = 128 * 4, .priority = osPriorityNormal,
    });

    state_machine_task_create();
    watchdog_task_create();
    lora_beacon_task_create();
    lora_rx_task_create();
    cloud_task_create();
    clear_task_create();

    ESP_LOGI(TAG, "All tasks created, starting scheduler...");
    osKernelStart();
}
