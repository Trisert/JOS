#ifndef SIM_SCENARIOS_H
#define SIM_SCENARIOS_H

#include <stdint.h>
#include "sim_state.h"

typedef struct sim_scenario {
    const char *name;
    const char *desc;
    sim_state_t *state;
    uint32_t elapsed_ms;
    uint32_t phase;
    bool active;
    void (*reset)(void);
    void (*start)(void);
    void (*step)(uint32_t dt_ms);
} sim_scenario_t;

void sim_scenarios_init(sim_state_t *state);
sim_scenario_t *sim_scenario_find(const char *name);

extern sim_scenario_t *sim_scenario_list[];

#endif
