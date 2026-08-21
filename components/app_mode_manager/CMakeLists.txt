idf_component_register(
    SRCS "app_mode_manager.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash log freertos
)
