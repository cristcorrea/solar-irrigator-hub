idf_component_register(
    SRCS "hub_station.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        esp_partition
        spiffs
        mqtt
        mqtt_manager
        lwip
        blufi_manager
        CJSON
        esfera_manager
        time_sync
)
