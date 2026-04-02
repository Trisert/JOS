#ifndef CRYSTALS_H
#define CRYSTALS_H

#include <stdint.h>

/* ---------- Pin assignments (map to CubeIDE GPIO config) ----------
 *  CRYSTALS_EN      → PC0   (3V potential difference on/off)
 *  CRYSTALS_HEATER  → PC1   (5 mW copper coil heater)
 *  CRYSTALS_CAM_TRIG → PC2  (camera trigger, active-low pulse)
 *  Photodiode front → ADC1 CH4 (PA3 — incident light)
 *  Photodiode rear  → ADC1 CH5 (if wired — transmitted light)
 * ------------------------------------------------------------------ */

#define CRYSTALS_EN_PORT          GPIOC
#define CRYSTALS_EN_PIN           GPIO_PIN_0
#define CRYSTALS_HEATER_PORT      GPIOC
#define CRYSTALS_HEATER_PIN       GPIO_PIN_1
#define CRYSTALS_CAM_TRIG_PORT    GPIOC
#define CRYSTALS_CAM_TRIG_PIN     GPIO_PIN_2

#define CRYSTALS_PD_FRONT_CH      ADC_CHANNEL_4
#define CRYSTALS_PD_REAR_CH       ADC_CHANNEL_5

/* Heater hysteresis (units: 0.1 °C) */
#define CRYSTALS_HEATER_ON_TEMP   50    /* turn heater on  at ≤ 5.0 °C */
#define CRYSTALS_HEATER_OFF_TEMP  70    /* turn heater off at ≥ 7.0 °C */

/* ---------- Types ---------- */
typedef struct {
    uint8_t  enabled;       /* device on/off */
    uint8_t  heater_on;    /* heater state */
    uint8_t  cam_setting;  /* 0=default, 1=custom */
    uint16_t pd_front_mv;  /* front photodiode (mV) */
    uint16_t pd_rear_mv;   /* rear photodiode (mV) */
    uint8_t  obscuration;  /* 0-100 % */
} crystals_status_t;

/* ---------- Public API ---------- */

void crystals_init(void);

/* Turn CRYSTALS on/off */
void crystals_enable(uint8_t on);
uint8_t crystals_is_enabled(void);

/* Heater management — call periodically with temperature in 0.1 °C */
void crystals_manage_heater(int16_t temp_c);

/* Pulse camera trigger for one image capture */
void crystals_take_photo(void);

/* Read photodiodes, compute obscuration %, return it */
uint8_t crystals_read_obscuration(void);

/* Full status snapshot */
crystals_status_t crystals_get_status(void);

#endif /* CRYSTALS_H */
