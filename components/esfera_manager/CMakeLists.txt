idf_component_register(SRCS "esfera_manager.c" "telemetry_log.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES CJSON
                       REQUIRES log nvs_flash freertos esp_partition esp_timer heap time_sync)
