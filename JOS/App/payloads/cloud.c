#include "cloud.h"
#include "memory.h"
#include "state_machine.h"
#include "MAX11128.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* ---------- Private state ---------- */
static cloud_sample_t last_sample;
static MAX11128_t cloud_adc;
static uint16_t baseline[CLOUD_FACES][CLOUD_STRIPES]; /* first-read baseline */

/* CLOUD ADC CS pin on SPI1 — PB4 per SATPF */
#define CLOUD_CS_PORT  GPIOB
#define CLOUD_CS_PIN   GPIO_PIN_4

extern SPI_HandleTypeDef hspi1;

/* ---------- Threshold for breach detection ---------- */
/* If ADC reading deviates > CLOUD_THRESH from baseline, stripe is breached */
#define CLOUD_THRESH  500  /* raw ADC units (~25% of full-scale) */

/* ---------- Internal helpers ---------- */

static void cloud_adc_init(void)
{
    cloud_adc.spiHandle = &hspi1;
    cloud_adc.csPort    = CLOUD_CS_PORT;
    cloud_adc.csPin     = CLOUD_CS_PIN;
    cloud_adc.VREF      = MAX1112XVREF;

    MAX11128_Initialize(&cloud_adc, &hspi1);
    MAX11128_ADC_Config(&cloud_adc);
    MAX11128_ADC_Uni_Setup(&cloud_adc);
}

/* Read all 16 channels for one face (face 0 = AIN0..AIN15) */
static void cloud_read_face(uint8_t face, uint16_t raw[CLOUD_STRIPES])
{
    /* TODO: if two faces share ADC via mux, select face first via GPIO */
    (void)face;

    for (int i = 0; i < CLOUD_STRIPES; i++) {
        raw[i] = MAX11128_ADC_ReadRawCH(&cloud_adc, (uint8_t)i);
    }
}

/* ---------- Public API ---------- */

void cloud_init(void)
{
    memset(&last_sample, 0, sizeof(last_sample));
    memset(baseline, 0, sizeof(baseline));
    cloud_adc_init();

    /* Acquire baseline from both faces */
    for (int f = 0; f < CLOUD_FACES; f++) {
        cloud_read_face(f, baseline[f]);
    }
}

int cloud_acquire(cloud_sample_t *out)
{
    int new_breaches = 0;
    uint16_t raw[CLOUD_STRIPES];
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    out->timestamp = now;

    for (int f = 0; f < CLOUD_FACES; f++) {
        cloud_read_face(f, raw);

        for (int s = 0; s < CLOUD_STRIPES; s++) {
            int32_t delta = (int32_t)raw[s] - (int32_t)baseline[f][s];
            if (delta < 0) delta = -delta;

            int breached = (delta > CLOUD_THRESH) ? 1 : 0;

            /* Only mark first breach */
            if (breached && !out->face[f].stripes[s].breached) {
                out->face[f].stripes[s].breached = 1;
                out->face[f].stripes[s].timestamp = now;
                new_breaches++;
            } else if (!breached) {
                out->face[f].stripes[s].breached = 0;
                out->face[f].stripes[s].timestamp = 0;
            }
        }
    }

    /* Copy to last_sample */
    memcpy(&last_sample, out, sizeof(last_sample));

    /* Write breached rows to FRAM via cyclic buffer */
    if (new_breaches > 0) {
        cyclic_buffer_write((const uint8_t *)out, sizeof(cloud_sample_t));
    }

    return new_breaches;
}

const cloud_sample_t *cloud_last_sample(void)
{
    return &last_sample;
}

/* ---------- CLOUD FreeRTOS task ---------- */

static void cloud_task(void *arg)
{
    (void)arg;
    cloud_sample_t sample;

    for (;;) {
        /* Only acquire when in ACTIVE state */
        if (state_machine_get_state() == STATE_ACTIVE) {
            cloud_acquire(&sample);
        }

        /* Check once per orbit (~90 min) — placeholder interval */
        osDelay(pdMS_TO_TICKS(90UL * 60 * 1000));
    }
}

osThreadId_t cloud_task_create(void)
{
    static const osThreadAttr_t attrs = {
        .name       = "cloud",
        .stack_size = 256 * 4,
        .priority   = osPriorityBelowNormal,
    };
    return osThreadNew(cloud_task, NULL, &attrs);
}
