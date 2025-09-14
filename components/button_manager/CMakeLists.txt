idf_component_register(SRCS "button_manager.c"
                        INCLUDE_DIRS "."
                        REQUIRES driver freertos log esp_system nvs_flash esp_timer)