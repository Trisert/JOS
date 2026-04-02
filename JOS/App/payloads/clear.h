#ifndef CLEAR_H
#define CLEAR_H

#include <stdint.h>
#include "cmsis_os.h"

/* ---------- Pin assignments (SATPF verified) ----------
 *  +Z face LEDs:  PE10 (colour A / white), PE12 (colour B / IR)
 *  -Z face LEDs:  PE14 (single bank)
 *  +Z photodiode: ADC1 CH8
 *  -Z photodiode: ADC1 CH9
 * ------------------------------------------------------ */

#define CLEAR_LED_FZ_A_PORT   GPIOE
#define CLEAR_LED_FZ_A_PIN    GPIO_PIN_10
#define CLEAR_LED_FZ_B_PORT   GPIOE
#define CLEAR_LED_FZ_B_PIN    GPIO_PIN_12
#define CLEAR_LED_FmZ_A_PORT  GPIOE
#define CLEAR_LED_FmZ_A_PIN   GPIO_PIN_14

#define CLEAR_PD_FZ_CH        ADC_CHANNEL_8
#define CLEAR_PD_FmZ_CH       ADC_CHANNEL_9

/* Max burst samples (1 Hz × 90 min = 5400) */
#define CLEAR_BURST_MAX       5400

/* ---------- Types ---------- */

typedef enum {
    CLEAR_FACE_Z  = 0,
    CLEAR_FACE_MZ = 1,
    CLEAR_FACE_COUNT
} clear_face_t;

typedef enum {
    CLEAR_LED_OFF   = 0,
    CLEAR_LED_WHITE = 1,
    CLEAR_LED_IR    = 2,
} clear_led_colour_t;

typedef struct {
    uint8_t  led_on[CLEAR_FACE_COUNT];
    uint8_t  led_colour[CLEAR_FACE_COUNT];
    uint16_t pd_mv[CLEAR_FACE_COUNT];
    uint16_t burst_count;
    uint8_t  burst_active;
} clear_status_t;

/* Single burst sample — written to FRAM */
typedef struct __attribute__((packed)) {
    uint32_t timestamp_ms;
    uint16_t pd_fz_mv;
    uint16_t pd_fmz_mv;
} clear_burst_point_t;

/* ---------- Public API ---------- */

void clear_init(void);

/* LED control */
void clear_led_set(clear_face_t face, clear_led_colour_t colour);
void clear_led_off(clear_face_t face);

/* Read one photodiode (mV) */
uint16_t clear_read_pd(clear_face_t face);

/* Burst acquisition (one orbit on command) */
void clear_burst_start(void);
void clear_burst_stop(void);

/* Status snapshot */
clear_status_t clear_get_status(void);

/* FreeRTOS task */
osThreadId_t clear_task_create(void);

#endif /* CLEAR_H */
