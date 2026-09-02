#include "esfera_manager.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "esp_log.h"
#include "cJSON.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if 0 /* Implementación de blob NVS retirada en etapa 2. */
#define MAX_ENTRADAS 32
#define TELEMETRY_NAMESPACE "telemetry"
#define TELEMETRY_KEY       "pending"
#define TELEMETRY_MAGIC     0x48554254UL
#define TELEMETRY_VERSION   1

typedef struct {
    float humedad;
    float temperatura;
    float voltaje;
    uint8_t riego;
    char mac[13];
    char timestamp[20];
} esfera_data_t;

static const char *TAG = "ESFERA_MANAGER";
static esfera_data_t buffer[MAX_ENTRADAS];
static size_t buffer_index = 0;
static SemaphoreHandle_t buffer_mutex = NULL;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    esfera_data_t entries[MAX_ENTRADAS];
} telemetry_blob_t;

static esp_err_t persist_entries_locked(const esfera_data_t *entries, size_t count)
{
    telemetry_blob_t blob = {
        .magic = TELEMETRY_MAGIC,
        .version = TELEMETRY_VERSION,
        .count = (uint16_t)count,
    };
    if (count > 0) {
        memcpy(blob.entries, entries, count * sizeof(entries[0]));
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(TELEMETRY_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, TELEMETRY_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
#endif

static bool valid_mac(const char *mac)
{
    if (mac == NULL || strlen(mac) != 12) {
        return false;
    }
    for (size_t i = 0; i < 12; ++i) {
        if (!((mac[i] >= '0' && mac[i] <= '9') ||
              (mac[i] >= 'A' && mac[i] <= 'F') ||
              (mac[i] >= 'a' && mac[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool normalize_mac(const char *input, char output[13])
{
    if (input == NULL || output == NULL) {
        return false;
    }
    size_t index = 0;
    for (size_t i = 0; input[i] != '\0'; ++i) {
        if (input[i] == ':') {
            continue;
        }
        if (index >= 12 || !((input[i] >= '0' && input[i] <= '9') ||
                             (input[i] >= 'A' && input[i] <= 'F') ||
                             (input[i] >= 'a' && input[i] <= 'f'))) {
            return false;
        }
        output[index++] = (input[i] >= 'a' && input[i] <= 'f')
                              ? (char)(input[i] - 'a' + 'A')
                              : input[i];
    }
    output[index] = '\0';
    return index == 12;
}

#if 0 /* Reemplazado por telemetry_log.c; se conserva temporalmente para aislar configuración NVS. */
esp_err_t esfera_manager_init(void) {
    if (buffer_mutex == NULL) {
        buffer_mutex = xSemaphoreCreateMutex();
        if (buffer_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    buffer_index = 0;
    memset(buffer, 0, sizeof(buffer));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(TELEMETRY_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    telemetry_blob_t blob;
    size_t blob_size = sizeof(blob);
    err = nvs_get_blob(handle, TELEMETRY_KEY, &blob, &blob_size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (blob_size != sizeof(blob) || blob.magic != TELEMETRY_MAGIC ||
        blob.version != TELEMETRY_VERSION || blob.count > MAX_ENTRADAS) {
        ESP_LOGE(TAG, "Datos pendientes NVS incompatibles o corruptos; se reinicia la cola lógica");
        return ESP_OK;
    }

    buffer_index = blob.count;
    memcpy(buffer, blob.entries, buffer_index * sizeof(buffer[0]));
    ESP_LOGI(TAG, "%u lecturas pendientes recuperadas de NVS", (unsigned)buffer_index);
    return ESP_OK;
}

//Agrega lecturas de esferas en la memoria 
esp_err_t esfera_manager_add(const char *raw_payload, const char *mac_origen) {
    if (raw_payload == NULL || !valid_mac(mac_origen)) {
        return ESP_ERR_INVALID_ARG;
    }

    float h, t, v;
    int r;
    char mac_payload[13];
    char trailing;
    if (sscanf(raw_payload, "%f,%f,%f,%d %12s %c", &h, &t, &v, &r, mac_payload, &trailing) != 5 ||
        !isfinite(h) || !isfinite(t) || !isfinite(v) ||
        h < 0.0f || h > 100.0f || t < -50.0f || t > 100.0f ||
        v < 0.0f || v > 20.0f || (r != 0 && r != 1) ||
        !valid_mac(mac_payload) || strcasecmp(mac_payload, mac_origen) != 0) {
        ESP_LOGW(TAG, "Telemetría inválida o MAC declarada distinta del remitente");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(buffer_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (buffer_index >= MAX_ENTRADAS) {
        ESP_LOGW(TAG, "⚠️ Buffer lleno, descartando entrada");
        xSemaphoreGive(buffer_mutex);
        return ESP_ERR_NO_MEM;
    }

    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    esfera_data_t *entrada = &buffer[buffer_index];
    entrada->humedad = h;
    entrada->temperatura = t;
    entrada->voltaje = v;
    entrada->riego = (uint8_t)r;
    memcpy(entrada->mac, mac_origen, 12);
    entrada->mac[12] = '\0';
    strftime(entrada->timestamp, sizeof(entrada->timestamp), "%Y-%m-%dT%H:%M:%S", &timeinfo);

    esp_err_t err = persist_entries_locked(buffer, buffer_index + 1);
    if (err != ESP_OK) {
        memset(entrada, 0, sizeof(*entrada));
        xSemaphoreGive(buffer_mutex);
        ESP_LOGE(TAG, "No se pudo persistir telemetría: %s", esp_err_to_name(err));
        return err;
    }
    buffer_index++;

    ESP_LOGI(TAG, "🟢 Entrada agregada: MAC=%s H=%.1f T=%.1f V=%.2f R=%d TS=%s",
             entrada->mac, h, t, v, r, entrada->timestamp);
    xSemaphoreGive(buffer_mutex);
    return ESP_OK;
}

char *esfera_manager_generate_json(size_t *entry_count) {
    if (entry_count != NULL) {
        *entry_count = 0;
    }
    if (xSemaphoreTake(buffer_mutex, portMAX_DELAY) != pdTRUE) {
        return NULL;
    }
    cJSON *root = cJSON_CreateArray();
    if (root == NULL) {
        xSemaphoreGive(buffer_mutex);
        return NULL;
    }

    for (size_t i = 0; i < buffer_index; i++) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            cJSON_Delete(root);
            xSemaphoreGive(buffer_mutex);
            return NULL;
        }
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

    if (json_string != NULL && entry_count != NULL) {
        *entry_count = buffer_index;
    }
    xSemaphoreGive(buffer_mutex);

    return json_string; // 🔁 Recordá: hay que liberar con free() luego de publicar
}

esp_err_t esfera_manager_remove_oldest(size_t count) {
    if (xSemaphoreTake(buffer_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (count > buffer_index) {
        count = buffer_index;
    }

    size_t remaining = buffer_index - count;
    esp_err_t err = persist_entries_locked(buffer + count, remaining);
    if (err == ESP_OK) {
        memmove(buffer, buffer + count, remaining * sizeof(buffer[0]));
        memset(buffer + remaining, 0, count * sizeof(buffer[0]));
        buffer_index = remaining;
    }
    xSemaphoreGive(buffer_mutex);
    return err;
}

size_t esfera_manager_count(void) {
    if (xSemaphoreTake(buffer_mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }
    size_t count = buffer_index;
    xSemaphoreGive(buffer_mutex);
    return count;
}

void esfera_manager_clear(void) {
    size_t count = esfera_manager_count();
    esp_err_t err = esfera_manager_remove_oldest(count);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "🧹 Buffer de esferas limpiado");
    } else {
        ESP_LOGE(TAG, "No se pudo limpiar telemetría: %s", esp_err_to_name(err));
    }
}
#endif

esp_err_t esfera_manager_store_config(const char *payload, size_t payload_len, char normalized_mac[13])
{
    if (payload == NULL || payload_len == 0 || payload_len >= 256 || normalized_mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *json = cJSON_ParseWithLength(payload, payload_len);
    if (json == NULL || !cJSON_IsObject(json)) {
        cJSON_Delete(json);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *mac_item = cJSON_GetObjectItemCaseSensitive(json, "MACSLAVE");
    if (!cJSON_IsString(mac_item) || !normalize_mac(mac_item->valuestring, normalized_mac)) {
        cJSON_Delete(json);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(json, "tok");
    cJSON_DeleteItemFromObjectCaseSensitive(json, "Data");

    char *canonical_json = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (canonical_json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* ESP-NOW v1 admite 250 bytes; se reserva margen para el campo ts añadido al enviar. */
    if (strlen(canonical_json) + 20 > 250) {
        free(canonical_json);
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI("ESFERA_MANAGER", "Configuración saneada para %s: %u bytes (sin tok/Data)",
             normalized_mac, (unsigned)strlen(canonical_json));

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("config_store", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, normalized_mac, canonical_json);
    }

    if (err == ESP_OK) {
        char ack_key[14];
        snprintf(ack_key, sizeof(ack_key), "a%s", normalized_mac);
        err = nvs_set_u8(handle, ack_key, 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    free(canonical_json);
    return err;
}

char *esfera_manager_cfg_status_json(void)
{
    enum { MAX_ITEMS = 20, ITEM_BUDGET = 64, BASE_BUDGET = 64 };
    size_t capacity = BASE_BUDGET + MAX_ITEMS * ITEM_BUDGET;
    char *json = malloc(capacity);
    if (json == NULL) return NULL;

    size_t used = (size_t)snprintf(json, capacity,
                                  "{\"cmd\":\"get_cfg_status\",\"status\":\"ok\",\"items\":[");
    nvs_handle_t handle = 0;
    esp_err_t open_err = nvs_open("config_store", NVS_READONLY, &handle);
    if (open_err != ESP_OK && open_err != ESP_ERR_NVS_NOT_FOUND) {
        free(json);
        return NULL;
    }

    nvs_iterator_t iterator = NULL;
    esp_err_t iter_err = nvs_entry_find(NVS_DEFAULT_PART_NAME, "config_store", NVS_TYPE_U8,
                                        &iterator);
    size_t count = 0;
    while (iter_err == ESP_OK && iterator != NULL && count < MAX_ITEMS) {
        nvs_entry_info_t info;
        nvs_entry_info(iterator, &info);
        size_t key_len = strlen(info.key);
        bool ack_key = key_len == 13 && info.key[0] == 'a';
        for (size_t i = 1; ack_key && i < key_len; ++i) {
            char c = info.key[i];
            ack_key = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                      (c >= 'a' && c <= 'f');
        }
        if (ack_key && handle != 0) {
            uint8_t applied = 0;
            if (nvs_get_u8(handle, info.key, &applied) == ESP_OK) {
                int written = snprintf(json + used, capacity - used,
                                       "%s{\"MACSLAVE\":\"%s\",\"applied\":%s}",
                                       count == 0 ? "" : ",", info.key + 1,
                                       applied == 1 ? "true" : "false");
                if (written < 0 || (size_t)written >= capacity - used) {
                    nvs_release_iterator(iterator);
                    if (handle != 0) nvs_close(handle);
                    free(json);
                    return NULL;
                }
                used += (size_t)written;
                ++count;
            }
        }
        iter_err = nvs_entry_next(&iterator);
    }
    if (iterator != NULL) nvs_release_iterator(iterator);
    if (handle != 0) nvs_close(handle);
    if (snprintf(json + used, capacity - used, "]}") >= (int)(capacity - used)) {
        free(json);
        return NULL;
    }
    return json;
}

esp_err_t esfera_manager_register_mac(const char *mac) {
    if (!valid_mac(mac)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("esferas", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    size_t len = 0;
    err = nvs_get_str(handle, mac, NULL, &len);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // La MAC no existe, la guardamos como "registered"
        err = nvs_set_str(handle, mac, "registered");
        if (err == ESP_OK) {
            err = nvs_commit(handle);
            ESP_LOGI("ESFERA_MANAGER", "✅ Nueva esfera registrada en NVS: %s", mac);
        }
    } else if (err == ESP_OK) {
        // Ya estaba registrada
        ESP_LOGD("ESFERA_MANAGER", "ℹ️ Esfera %s ya estaba registrada en NVS", mac);
    }

    nvs_close(handle);
    return err;
}

bool esfera_manager_is_registered(const char *mac)
{
    if (!valid_mac(mac)) {
        return false;
    }
    nvs_handle_t handle;
    if (nvs_open("esferas", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_str(handle, mac, NULL, &len);
    nvs_close(handle);
    return err == ESP_OK;
}
