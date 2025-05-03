idf_component_register(
    SRCS "blufi_manager.c" "blufi_security.c" "blufi_init.c"
    INCLUDE_DIRS "." "$ENV{IDF_PATH}/components/bt/common/api/include/api"
    PRIV_REQUIRES bt nvs_flash esp_wifi mbedtls
)
