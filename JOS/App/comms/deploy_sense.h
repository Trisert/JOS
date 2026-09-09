#ifndef DEPLOY_SENSE_H
#define DEPLOY_SENSE_H

/* deploy_sense — DEPLOY_SENSE / LoRa_NRST multiplexed GPIO (PB1).
 *
 * SPF v3 3.7.5.3.1 p.95: ONE OBC GPIO is shared between the antenna
 * deployment switch (DEPLOY_SENSE, COMMS connector pin 19) and the SX1268
 * reset (LoRa_NRST, COMMS connector pin 4). The two roles are mutually
 * exclusive in time:
 *   - LoRa_NRST : output push-pull, idle HIGH (SX1268 reset is active-low).
 *   - DEPLOY_SENSE read: temporarily reconfigure PB1 as input + pull-up,
 *     sample, then IMMEDIATELY restore output HIGH so the line never floats
 *     into a spurious radio reset.
 *
 * OWNERSHIP RULE: call deploy_read_raw() only while the radio is idle
 * (never between lora_tx() and lora_tx_wait_done(), never from the DIO1
 * ISR path). The pin is left as output HIGH on return either way.
 *
 * Switch polarity: the deploy switch pulls PB1 to GND through the harness
 * when the antenna has fired, so DEPLOYED reads 0 (LOW) and STOWED reads 1
 * (HIGH, pull-up). If the harness inverts this, flip DEPLOY_DEPLOYED_LEVEL
 * in one place below.
 */

#define DEPLOY_DEPLOYED_LEVEL 0

/* Configure PB1 as output, idle HIGH (radio NOT in reset). */
void deploy_mux_init(void);

/* Sample the deploy switch (reconfigures PB1, restores output HIGH before
 * returning). Returns the raw level 0/1, negative on bus error. */
int deploy_read_raw(void);

/* 1 when the switch reports DEPLOYED, 0 when STOWED, negative on error. */
int deploy_is_deployed(void);

/* SX1268 reset helpers (same pin, output role). pulse: active-low 200 us
 * (>100 us per SX1268 datasheet). leaves the pin HIGH. */
void deploy_nrst_assert(void);
void deploy_nrst_release(void);
void deploy_nrst_pulse(void);

#endif /* DEPLOY_SENSE_H */
