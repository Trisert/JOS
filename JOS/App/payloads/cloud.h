#ifndef CLOUD_H
#define CLOUD_H

#include <stdint.h>
#include <stddef.h>
#include "cmsis_os.h"

/* ---------- Constants ---------- */
#define CLOUD_FACES          2    /* +Y and -Y */
#define CLOUD_STRIPES        16   /* resistive stripes per face */
#define CLOUD_FACE_DATA_SZ   (CLOUD_STRIPES * (1 + sizeof(uint32_t))) /* bool + timestamp per stripe */

/* Per-stripe breach record: 1 byte status + 4 byte timestamp = 5 bytes */
typedef struct {
    uint8_t  breached;    /* 0 = intact, 1 = breached */
    uint32_t timestamp;   /* first-breach tick (ms since boot) */
} cloud_stripe_t;

/* Full face acquisition result */
typedef struct {
    cloud_stripe_t stripes[CLOUD_STRIPES];
} cloud_face_t;

/* Complete CLOUD sample (both faces) */
typedef struct {
    cloud_face_t face[CLOUD_FACES];
    uint32_t     timestamp;       /* sample tick */
} cloud_sample_t;

/* ---------- Public API ---------- */

/* Initialise CLOUD driver (ADC config, GPIO) */
void cloud_init(void);

/* Acquire one sample from both faces.
 * Reads all 16 stripes per face via MAX11128 ADC,
 * compares against baseline, marks breaches.
 * Returns number of newly breached stripes. */
int cloud_acquire(cloud_sample_t *out);

/* Get most recent sample (non-blocking copy) */
const cloud_sample_t *cloud_last_sample(void);

/* Create the periodic CLOUD FreeRTOS task (once per orbit or on command) */
osThreadId_t cloud_task_create(void);

#endif /* CLOUD_H */
