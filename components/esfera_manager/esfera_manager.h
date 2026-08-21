#pragma once

#include <inttypes.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    float humedad;
    float temperatura;
    float voltaje;
    uint8_t riego;
    char mac[13];         // "A085E369D6AC"
    char timestamp[20];   // "2025-04-22T14:00:00"
} esfera_data_t;

esp_err_t esfera_manager_init(void);
esp_err_t esfera_manager_add(const char *raw_payload, const char *mac_origen);
char *esfera_manager_generate_json(size_t *entry_count);
esp_err_t esfera_manager_remove_oldest(size_t count);
size_t esfera_manager_count(void);
void esfera_manager_clear(void);
esp_err_t esfera_manager_store_config(const char *payload, size_t payload_len, char normalized_mac[13]);
esp_err_t esfera_manager_register_mac(const char *mac);
bool esfera_manager_is_registered(const char *mac);
