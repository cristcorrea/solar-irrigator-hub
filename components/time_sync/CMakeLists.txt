idf_component_register(SRCS "time_sync.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_event log esp_netif)
