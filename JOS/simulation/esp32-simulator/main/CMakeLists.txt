idf_component_register(
    SRCS "main.c" "sim_sensors.c" "sim_scenarios.c"
    INCLUDE_DIRS "." "../shared"
    REQUIRES driver esp_timer
)
