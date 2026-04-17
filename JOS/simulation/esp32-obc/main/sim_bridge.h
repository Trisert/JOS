#ifndef SIM_BRIDGE_H
#define SIM_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include "sim_protocol.h"

int sim_bridge_send(const uint8_t *data, size_t len);
int sim_bridge_send_recv(const uint8_t *tx, uint8_t *rx, size_t len);
void sim_bridge_notify_gpio(const sim_gpio_state_t *gpio);

#endif
