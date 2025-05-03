#pragma once

#include <inttypes.h>

typedef struct {
    float humedad;
    float temperatura;
    float voltaje;
    uint8_t riego;
    char mac[13];         // "A085E369D6AC"
    char timestamp[20];   // "2025-04-22T14:00:00"
} esfera_data_t;

void esfera_manager_init(void);
void esfera_manager_add(const char *raw_payload, const char *mac_origen);
char *esfera_manager_generate_json(void);
void esfera_manager_clear(void);
