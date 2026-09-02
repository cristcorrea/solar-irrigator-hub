idf_component_register(
    SRCS "provisioning_manager.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_timer app_mode_manager
)
