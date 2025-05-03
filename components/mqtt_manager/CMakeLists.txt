idf_component_register(SRCS "mqtt_manager.c"
                       INCLUDE_DIRS "." 
                       REQUIRES mqtt nvs_flash esp_event esp_wifi
                       PRIV_REQUIRES main CJSON esfera_manager
                       EMBED_TXTFILES 
                       certificates/ca_cert.pem 
                       certificates/client_cert.pem 
                       certificates/client_key.pem)
