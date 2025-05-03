#include "esfera_manager.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "cJSON.h"

#define MAX_ENTRADAS 32

static const char *TAG = "ESFERA_MANAGER";
static esfera_data_t buffer[MAX_ENTRADAS];
static size_t buffer_index = 0;

void esfera_manager_init(void) {
    buffer_index = 0;
    memset(buffer, 0, sizeof(buffer));
}

void esfera_manager_add(const char *raw_payload, const char *mac_origen) {
    if (buffer_index >= MAX_ENTRADAS) {
        ESP_LOGW(TAG, "⚠️ Buffer lleno, descartando entrada");
        return;
    }

    float h, t, v;
    int r;
    char mac_final[13];
    if (sscanf(raw_payload, "%f,%f,%f,%d %12s", &h, &t, &v, &r, mac_final) != 5) {
        ESP_LOGW(TAG, "⚠️ Formato inválido: %s", raw_payload);
        return;
    }

    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    esfera_data_t *entrada = &buffer[buffer_index++];
    entrada->humedad = h;
    entrada->temperatura = t;
    entrada->voltaje = v;
    entrada->riego = (uint8_t)r;
    strncpy(entrada->mac, mac_final, sizeof(entrada->mac));
    strftime(entrada->timestamp, sizeof(entrada->timestamp), "%Y-%m-%dT%H:%M:%S", &timeinfo);

    ESP_LOGI(TAG, "🟢 Entrada agregada: MAC=%s H=%.1f T=%.1f V=%.2f R=%d TS=%s",
             entrada->mac, h, t, v, r, entrada->timestamp);
}

char *esfera_manager_generate_json(void) {
    cJSON *root = cJSON_CreateArray();

    for (size_t i = 0; i < buffer_index; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "mac", buffer[i].mac);
        cJSON_AddNumberToObject(item, "humedad", buffer[i].humedad);
        cJSON_AddNumberToObject(item, "temperatura", buffer[i].temperatura);
        cJSON_AddNumberToObject(item, "bateria", buffer[i].voltaje);
        cJSON_AddNumberToObject(item, "riego", buffer[i].riego);
        cJSON_AddStringToObject(item, "timestamp", buffer[i].timestamp);
        cJSON_AddItemToArray(root, item);
    }

    char *json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_string; // 🔁 Recordá: hay que liberar con free() luego de publicar
}

void esfera_manager_clear(void) {
    buffer_index = 0;
    memset(buffer, 0, sizeof(buffer));
    ESP_LOGI(TAG, "🧹 Buffer de esferas limpiado");
}
