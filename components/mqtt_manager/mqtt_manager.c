#include "mqtt_manager.h"
#include "esp_log.h"
#include <string.h>
#include "mqtt_secrets.h"
#include "hub_station.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_mac.h"
#include "esfera_manager.h"

#define TAG "MQTT_MANAGER"

static char topic_public[30] = {0};
static char topic_suscripcion[64] = {0};
extern char mac_local[13]; // Formato XX:XX:XX:XX:XX:XX

static esp_mqtt_client_handle_t client = NULL;

// --- Certificados (definidos en mqtt_secrets.h) ---
extern const uint8_t ca_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t client_cert_pem_start[] asm("_binary_client_cert_pem_start");
extern const uint8_t client_key_pem_start[] asm("_binary_client_key_pem_start");

// --- Configuración estándar ---
static const char *default_config_json =
    "{\"MACHUB\":\"%s\",\"MACSLAVE\":\"%s\","
    "\"colorLED\":16777215,"
    "\"riegoAuto\":0,"
    "\"diasRiego\":0,"
    "\"horaRiego\":\"08:00\","
    "\"ml\":100}";

// --- Prototipos privados ---
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event);
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void procesar_configuracion_esfera(const char *payload);
static void intentar_enviar_configuracion_a_esfera(const char *mac_str, const uint8_t *mac_bin);

// ============================================================
//   ENVÍO DE CONFIGURACIÓN A UNA ESFERA
// ============================================================
static void intentar_enviar_configuracion_a_esfera(const char *mac_str, const uint8_t *mac_bin)
{
    char mac_clean[13];
    int j = 0;
    for (int i = 0; mac_str[i] && j < 12; i++) {
        if (mac_str[i] != ':') mac_clean[j++] = mac_str[i];
    }
    mac_clean[j] = '\0';

    ESP_LOGI(TAG, "🔎 Buscando configuración para %s", mac_clean);

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("config_store", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "⚠️ No se pudo abrir NVS, usando configuración estándar");
        char buffer[256];
        snprintf(buffer, sizeof(buffer), default_config_json, mac_local, mac_str);
        esp_now_send(mac_bin, (uint8_t *)buffer, strlen(buffer));
        ESP_LOGI(TAG, "✅ Configuración estándar enviada a %s", mac_str);
        return;
    }

    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, mac_clean, NULL, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "ℹ️ No hay configuración en NVS, usando estándar para %s", mac_clean);
        char buffer[256];
        snprintf(buffer, sizeof(buffer), default_config_json, mac_local, mac_str);
        esp_now_send(mac_bin, (uint8_t *)buffer, strlen(buffer));
        ESP_LOGI(TAG, "✅ Configuración estándar enviada a %s", mac_str);
        nvs_close(nvs_handle);
        return;
    }

    char *config_json = malloc(required_size);
    if (!config_json) {
        ESP_LOGE(TAG, "❌ Error asignando memoria para JSON");
        nvs_close(nvs_handle);
        return;
    }

    err = nvs_get_str(nvs_handle, mac_clean, config_json, &required_size);
    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Error leyendo configuración para %s", mac_str);
        free(config_json);
        return;
    }

    esp_err_t send_result = esp_now_send(mac_bin, (uint8_t *)config_json, strlen(config_json));
    if (send_result == ESP_OK) {
        ESP_LOGI(TAG, "✅ Configuración enviada a %s", mac_str);
        ESP_LOGI(TAG, "%s", config_json);
    } else {
        ESP_LOGE(TAG, "❌ Fallo al enviar configuración a %s", mac_str);
    }

    free(config_json);
}

// ============================================================
//   GUARDAR CONFIGURACIÓN RECIBIDA DESDE MQTT
// ============================================================
static void procesar_configuracion_esfera(const char *payload)
{
    cJSON *json = cJSON_Parse(payload);
    if (!json) {
        ESP_LOGE(TAG, "❌ Error al parsear JSON");
        return;
    }

    cJSON *data_flag = cJSON_GetObjectItem(json, "Data");
    if (cJSON_IsBool(data_flag) && cJSON_IsTrue(data_flag)) {
        ESP_LOGI(TAG, "📲 Petición de datos (se maneja en handler)");
        cJSON_Delete(json);
        return;
    }

    cJSON *mac_slave = cJSON_GetObjectItem(json, "MACSLAVE");
    if (!cJSON_IsString(mac_slave)) {
        ESP_LOGE(TAG, "❌ MACSLAVE inválido o ausente");
        cJSON_Delete(json);
        return;
    }

    char *json_string = cJSON_PrintUnformatted(json);
    if (!json_string) {
        ESP_LOGE(TAG, "❌ No se pudo convertir JSON a string");
        cJSON_Delete(json);
        return;
    }

    char mac_clean[13];
    int j = 0;
    for (int i = 0; mac_slave->valuestring[i] && j < 12; i++) {
        if (mac_slave->valuestring[i] != ':') mac_clean[j++] = mac_slave->valuestring[i];
    }
    mac_clean[j] = '\0';

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("config_store", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Error abriendo NVS: %s", esp_err_to_name(err));
        free(json_string);
        cJSON_Delete(json);
        return;
    }

    err = nvs_set_str(nvs_handle, mac_clean, json_string);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
        ESP_LOGI(TAG, "💾 Configuración almacenada para %s", mac_slave->valuestring);
    } else {
        ESP_LOGE(TAG, "❌ Error escribiendo en NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    free(json_string);
    cJSON_Delete(json);
}

// ============================================================
//   CALLBACK EVENTOS MQTT
// ============================================================
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "🔌 Conectado al broker MQTT");
        vTaskDelay(pdMS_TO_TICKS(500));
        mqtt_manager_suscribirse(topic_suscripcion);
        break;

    case MQTT_EVENT_DATA: {
        ESP_LOGI(TAG, "📥 Mensaje recibido:");
        ESP_LOGI(TAG, "📌 Topic: %.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "📝 Data: %.*s", event->data_len, event->data);

        char payload[256];
        int len = event->data_len < sizeof(payload) - 1 ? event->data_len : sizeof(payload) - 1;
        memcpy(payload, event->data, len);
        payload[len] = '\0';

        cJSON *json = cJSON_Parse(payload);
        if (!json) {
            ESP_LOGW(TAG, "⚠️ JSON inválido");
            break;
        }

        cJSON *data_flag = cJSON_GetObjectItem(json, "Data");
        if (cJSON_IsTrue(data_flag)) {
            ESP_LOGI(TAG, "📲 Petición de datos recibida. Enviando...");

            char *json_out = esfera_manager_generate_json();
            esp_mqtt_client_publish(event->client, topic_public, json_out, 0, 1, 0);
            free(json_out);
            esfera_manager_clear();
        } else {
            ESP_LOGI(TAG, "⚙️ Configuración recibida");
            procesar_configuracion_esfera(payload);
        }

        cJSON_Delete(json);
        break;
    }

    default:
        break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    mqtt_event_handler_cb(event);
}

// ============================================================
//   FUNCIÓN PARA CALLBACK ESP-NOW
// ============================================================
void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
             recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
             recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5]);

    char payload[128] = {0};
    memcpy(payload, data, len < 127 ? len : 127);

    ESP_LOGI(TAG, "📥 Recibido de %s: %s", mac_str, payload);

    esfera_manager_add(payload, mac_str);

    esp_err_t err = esfera_manager_register_mac(mac_str);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✅ Nueva esfera registrada: %s", mac_str);
    }

    esp_now_peer_info_t peer = {
        .ifidx = WIFI_IF_STA,
        .encrypt = false};
    memcpy(peer.peer_addr, recv_info->src_addr, ESP_NOW_ETH_ALEN);

    if (!esp_now_is_peer_exist(recv_info->src_addr)) {
        esp_now_add_peer(&peer);
    } else {
        intentar_enviar_configuracion_a_esfera(mac_str, recv_info->src_addr);
    }
}

// ============================================================
//   INICIALIZACIÓN Y PUBLICACIÓN MQTT
// ============================================================
void mqtt_manager_init(void)
{
    snprintf(topic_public, sizeof(topic_public), "ismart/app/%s", mac_local);
    snprintf(topic_suscripcion, sizeof(topic_suscripcion), "ismart/hub/%s", mac_local);
    ESP_LOGI(TAG, "📡 Topic suscripción: %s", topic_suscripcion);
    ESP_LOGI(TAG, "📡 Topic publicación: %s", topic_public);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = MQTT_URI,
            .verification = {
                .certificate = (const char *)ca_cert_pem_start,
                .use_global_ca_store = false,
                .skip_cert_common_name_check = false,
            },
        },
        .credentials = {
            .username = MQTT_USERNAME,
            .client_id = mac_local,
            .authentication = {
                .password = MQTT_PASSWORD,
                .certificate = (const char *)client_cert_pem_start,
                .key = (const char *)client_key_pem_start,
            },
        },
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "❌ No se pudo inicializar el cliente MQTT");
        return;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
    ESP_LOGI(TAG, "✅ Cliente MQTT iniciado");
}

void mqtt_manager_publicar_datos(const char *mac, float temperatura, float humedad, float voltaje, int riego)
{
    char payload[256];
    snprintf(payload, sizeof(payload),
             "temperatura,device=%s value=%.2f\n"
             "humedad,device=%s value=%.2f\n"
             "vbat,device=%s value=%.2f\n"
             "riego,device=%s value=%d",
             mac, temperatura,
             mac, humedad,
             mac, voltaje,
             mac, riego);

    int msg_id = esp_mqtt_client_publish(client, topic_public, payload, 0, 1, 0);
    if (msg_id != -1) {
        ESP_LOGI(TAG, "✅ Publicado a %s: %s", topic_public, payload);
    } else {
        ESP_LOGE(TAG, "❌ Error al publicar en el broker MQTT");
    }
}

void mqtt_manager_suscribirse(char *topic)
{
    if (client) {
        ESP_LOGI(TAG, "🧭 Suscribiéndose al topic: %s", topic);
        int msj_id = esp_mqtt_client_subscribe(client, topic, 1);
        if (msj_id < 0) {
            ESP_LOGE(TAG, "❌ Falló la suscripción al topic: %d", msj_id);
        } else {
            ESP_LOGI(TAG, "📥 Suscripción exitosa con msg_id: %d", msj_id);
            hub_iniciar_espnow();
        }
    } else {
        ESP_LOGE(TAG, "❌ Cliente MQTT no inicializado");
    }
}

esp_mqtt_client_handle_t mqtt_manager_obtener_cliente(void)
{
    return client;
}
