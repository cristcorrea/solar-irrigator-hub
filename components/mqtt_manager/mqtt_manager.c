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
#include "freertos/queue.h"
#include <time.h>

#define TAG "MQTT_MANAGER"

typedef struct
{
    uint8_t src[6];
    uint16_t len;
    char payload[128];
} espnow_rx_msg_t;

static QueueHandle_t s_espnow_rx_q = NULL;
static void espnow_worker_task(void *arg);

// (si ya tienes esta función, reutilízala)
static void procesar_mensaje_ctrl_esfera(const char *mac_str, const char *payload);

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
static void procesar_mensaje_ctrl_esfera(const char *mac_str, const char *payload);

// Helper: convierte "A085E369D6AC" -> uint8_t[6]
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool mac_str_to_bytes(const char *mac_str, uint8_t out[6])
{
    if (!mac_str) return false;
    size_t len = strlen(mac_str);
    if (len != 12) return false;

    for (int i = 0; i < 6; i++) {
        int hi = hex_nibble(mac_str[2 * i]);
        int lo = hex_nibble(mac_str[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}


// --- Helpers ACK de configuración (NVS: "config_store") ---
static bool cfg_ack_is_applied(const char *mac_clean)
{
    nvs_handle_t h;
    uint8_t ack = 0;
    if (nvs_open("config_store", NVS_READONLY, &h) != ESP_OK)
        return false;
    char key[20];
    snprintf(key, sizeof(key), "ack_%s", mac_clean);
    esp_err_t err = nvs_get_u8(h, key, &ack);
    nvs_close(h);
    return (err == ESP_OK && ack == 1);
}

static void cfg_ack_set_applied(const char *mac_clean, bool applied)
{
    nvs_handle_t h;
    if (nvs_open("config_store", NVS_READWRITE, &h) != ESP_OK)
        return;
    char key[20];
    snprintf(key, sizeof(key), "ack_%s", mac_clean);
    nvs_set_u8(h, key, applied ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

// ============================================================
//   ENVÍO DE CONFIGURACIÓN A UNA ESFERA
// ============================================================
static void intentar_enviar_configuracion_a_esfera(const char *mac_str, const uint8_t *mac_bin)
{

    esp_err_t send_result;

    char mac_clean[13];
    int j = 0;
    for (int i = 0; mac_str[i] && j < 12; i++)
        if (mac_str[i] != ':')
            mac_clean[j++] = mac_str[i];
    mac_clean[j] = '\0';

    // --- NUEVO: no enviar si ya aplicada ---
    if (cfg_ack_is_applied(mac_clean))
    {
        ESP_LOGI(TAG, "🧩 (skip) Config de %s ya aplicada (ACK en NVS).", mac_str);
        return;
    }

    ESP_LOGI(TAG, "🔎 Buscando configuración para %s", mac_clean);
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("config_store", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "⚠️ No se pudo abrir NVS, usando configuración estándar");
        char buffer[256];
        snprintf(buffer, sizeof(buffer), default_config_json, mac_local, mac_str);

        time_t now = time(NULL);
        size_t used = strlen(buffer);
        snprintf(buffer + (used - 1), sizeof(buffer) - (used - 1),
                 ",\"ts\":%ld}", (long)now);

        esp_now_send(mac_bin, (uint8_t *)buffer, strlen(buffer));

        ESP_LOGI(TAG, "✅ Configuración estándar enviada a %s", mac_str);
        return;
    }

    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, mac_clean, NULL, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGI(TAG, "ℹ️ No hay configuración en NVS, usando estándar para %s", mac_clean);
        char buffer[256];
        snprintf(buffer, sizeof(buffer), default_config_json, mac_local, mac_str);

        time_t now = time(NULL);
        size_t used = strlen(buffer);
        snprintf(buffer + (used - 1), sizeof(buffer) - (used - 1),
                 ",\"ts\":%ld}", (long)now);

        esp_now_send(mac_bin, (uint8_t *)buffer, strlen(buffer));

        ESP_LOGI(TAG, "✅ Configuración estándar enviada a %s", mac_str);
        nvs_close(nvs_handle);
        return;
    }

    char *config_json = malloc(required_size);
    if (!config_json)
    {
        ESP_LOGE(TAG, "❌ Error asignando memoria para JSON");
        nvs_close(nvs_handle);
        return;
    }

    err = nvs_get_str(nvs_handle, mac_clean, config_json, &required_size);
    nvs_close(nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ Error leyendo configuración para %s", mac_str);
        free(config_json);
        return;
    }

    // esp_err_t send_result = esp_now_send(mac_bin, (uint8_t *)config_json, strlen(config_json));
    char *end = strrchr(config_json, '}');
    if (!end)
    {
        // JSON inesperado: envía tal cual
        send_result = esp_now_send(mac_bin, (uint8_t *)config_json, strlen(config_json));
        (void)send_result;
    }
    else
    {
        time_t now = time(NULL);
        size_t prefix = (size_t)(end - config_json); // hasta antes de '}'
        // margen suficiente: ,\"ts\":<10 dígitos>}
        char extra[32];
        int n = snprintf(extra, sizeof(extra), ",\"ts\":%ld}", (long)now);

        size_t out_len = prefix + (size_t)n;
        char *out = malloc(out_len + 1);
        if (!out)
        {
            send_result = esp_now_send(mac_bin, (uint8_t *)config_json, strlen(config_json));
            (void)send_result;
        }
        else
        {
            memcpy(out, config_json, prefix);
            memcpy(out + prefix, extra, (size_t)n);
            out[out_len] = '\0';
            send_result = esp_now_send(mac_bin, (uint8_t *)out, out_len);
            (void)send_result;
            free(out);
        }
    }

    if (send_result == ESP_OK)
    {
        ESP_LOGI(TAG, "✅ Configuración enviada a %s", mac_str);
        ESP_LOGI(TAG, "%s", config_json);
    }
    else
    {
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
    if (!json)
    {
        ESP_LOGE(TAG, "❌ Error al parsear JSON");
        return;
    }

    cJSON *data_flag = cJSON_GetObjectItem(json, "Data");
    if (cJSON_IsBool(data_flag) && cJSON_IsTrue(data_flag))
    {
        ESP_LOGI(TAG, "📲 Petición de datos (se maneja en handler)");
        cJSON_Delete(json);
        return;
    }

    cJSON *mac_slave = cJSON_GetObjectItem(json, "MACSLAVE");
    if (!cJSON_IsString(mac_slave))
    {
        ESP_LOGE(TAG, "❌ MACSLAVE inválido o ausente");
        cJSON_Delete(json);
        return;
    }

    char *json_string = cJSON_PrintUnformatted(json);
    if (!json_string)
    {
        ESP_LOGE(TAG, "❌ No se pudo convertir JSON a string");
        cJSON_Delete(json);
        return;
    }

    char mac_clean[13];
    int j = 0;
    for (int i = 0; mac_slave->valuestring[i] && j < 12; i++)
    {
        if (mac_slave->valuestring[i] != ':')
            mac_clean[j++] = mac_slave->valuestring[i];
    }
    mac_clean[j] = '\0';

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("config_store", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ Error abriendo NVS: %s", esp_err_to_name(err));
        free(json_string);
        cJSON_Delete(json);
        return;
    }

    err = nvs_set_str(nvs_handle, mac_clean, json_string);
    if (err == ESP_OK)
    {
        nvs_commit(nvs_handle);
        ESP_LOGI(TAG, "💾 Configuración almacenada para %s", mac_slave->valuestring);
        nvs_close(nvs_handle);
        cfg_ack_set_applied(mac_clean, false);
    }
    else
    {
        ESP_LOGE(TAG, "❌ Error escribiendo en NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
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
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "🔌 Conectado al broker MQTT");
        vTaskDelay(pdMS_TO_TICKS(500));
        mqtt_manager_suscribirse(topic_suscripcion);
        break;

    case MQTT_EVENT_DATA:
    {
        ESP_LOGI(TAG, "📥 Mensaje recibido:");
        ESP_LOGI(TAG, "📌 Topic: %.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "📝 Data: %.*s", event->data_len, event->data);

        char payload[256];
        int len = event->data_len < sizeof(payload) - 1 ? event->data_len : sizeof(payload) - 1;
        memcpy(payload, event->data, len);
        payload[len] = '\0';

        cJSON *json = cJSON_Parse(payload);
        if (!json)
        {
            ESP_LOGW(TAG, "⚠️ JSON inválido");
            break;
        }

        cJSON *data_flag = cJSON_GetObjectItem(json, "Data");
        if (cJSON_IsTrue(data_flag))
        {
            ESP_LOGI(TAG, "📲 Petición de datos recibida. Enviando...");

            char *json_out = esfera_manager_generate_json();
            esp_mqtt_client_publish(event->client, topic_public, json_out, 0, 1, 0);
            free(json_out);
            esfera_manager_clear();
        }
        else
        {
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

static void procesar_mensaje_ctrl_esfera(const char *mac_str, const char *payload)
{
    // Normalizar MAC sin ':'
    char mac_clean[13];
    int j = 0;
    for (int i = 0; mac_str[i] && j < 12; i++)
        if (mac_str[i] != ':')
            mac_clean[j++] = mac_str[i];
    mac_clean[j] = '\0';

    if (strncmp(payload, "CFG_OK", 6) == 0)
    {
        ESP_LOGI(TAG, "🟢 %s confirmó configuración (CFG_OK)", mac_str);
        cfg_ack_set_applied(mac_clean, true);

        if (client)
        {
            char json[128];
            snprintf(json, sizeof(json), "{\"MACSLAVE\":\"%s\",\"cfgAck\":\"ok\"}", mac_str);
            esp_mqtt_client_publish(client, topic_public, json, 0, 1, 0);
        }

        // --- NUEVO: ACK final de cierre de operación hacia la esfera ---
        uint8_t mac_bin[6];
        if (mac_str_to_bytes(mac_str, mac_bin))
        {
            if (!esp_now_is_peer_exist(mac_bin))
            {
                esp_now_peer_info_t peer = {0};
                peer.ifidx = WIFI_IF_STA;
                peer.encrypt = false;
                memcpy(peer.peer_addr, mac_bin, ESP_NOW_ETH_ALEN);
                esp_now_add_peer(&peer);
            }

            const char ack_msg[] = "ACK_END"; // Mensaje que la esfera debe esperar
            esp_err_t err = esp_now_send(mac_bin, (const uint8_t *)ack_msg, sizeof(ack_msg) - 1);
            if (err == ESP_OK)
            {
                ESP_LOGI(TAG, "🔁 ACK final de configuración enviado a %s (%s)", mac_str, ack_msg);
            }
            else
            {
                ESP_LOGW(TAG, "⚠️ Error enviando ACK final a %s: %s", mac_str, esp_err_to_name(err));
            }
        }
        else
        {
            ESP_LOGW(TAG, "⚠️ No se pudo convertir MAC '%s' a binario para enviar ACK final", mac_str);
        }
        // --- FIN NUEVO ---

        return;
    }

    if (strncmp(payload, "CFG_ERR", 7) == 0)
    {
        const char *razon = payload + 7;
        while (*razon == ':' || *razon == ' ')
            razon++;
        ESP_LOGW(TAG, "⚠️ %s reportó error de configuración: %s", mac_str, *razon ? razon : "sin detalle");
        // No marcamos ACK aplicado
        if (client)
        {
            char json[256];
            snprintf(json, sizeof(json),
                     "{\"MACSLAVE\":\"%s\",\"cfgAck\":\"err\",\"reason\":\"%s\"}",
                     mac_str, *razon ? razon : "unknown");
            esp_mqtt_client_publish(client, topic_public, json, 0, 1, 0);
        }
        return;
    }

    ESP_LOGI(TAG, "ℹ️ Control no reconocido de %s: %s", mac_str, payload);
}

// ============================================================
//   FUNCIÓN PARA CALLBACK ESP-NOW
// ============================================================
void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (!s_espnow_rx_q)
        return;

    espnow_rx_msg_t m = (espnow_rx_msg_t){0};
    memcpy(m.src, recv_info->src_addr, ESP_NOW_ETH_ALEN);
    m.len = (len < sizeof(m.payload) - 1) ? len : sizeof(m.payload) - 1;
    memcpy(m.payload, data, m.len);
    m.payload[m.len] = '\0';

    BaseType_t ok = xQueueSend(s_espnow_rx_q, &m, 0); // sin bloquear
    if (ok != pdTRUE)
    {
        ESP_LOGW(TAG,
                 "⚠️ Cola ESP-NOW llena, descartando paquete de %02X:%02X:%02X:%02X:%02X:%02X",
                 m.src[0], m.src[1], m.src[2], m.src[3], m.src[4], m.src[5]);
    }
}

static void espnow_worker_task(void *arg)
{
    espnow_rx_msg_t m;
    for (;;)
    {
        if (xQueueReceive(s_espnow_rx_q, &m, portMAX_DELAY) != pdTRUE)
            continue;

        char mac_str[13];
        snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
                 m.src[0], m.src[1], m.src[2], m.src[3], m.src[4], m.src[5]);

        // 1) Control de config (CFG_OK / CFG_ERR) para no romper el parser de datos
        if (strncmp(m.payload, "CFG_", 4) == 0)
        {
            procesar_mensaje_ctrl_esfera(mac_str, m.payload);
            continue;
        }

        // 1.b) Mensaje de emparejamiento: HELLO_HUB,<MAC_ESFERA>
        if (strncmp(m.payload, "HELLO_HUB,", 10) == 0)
        {
            ESP_LOGI(TAG, "🤝 HELLO_HUB recibido de %s: %s", mac_str, m.payload);

            // Registrar la esfera en NVS (si no existía)
            (void)esfera_manager_register_mac(mac_str);

            // Asegurar peer ESP-NOW
            if (!esp_now_is_peer_exist(m.src))
            {
                esp_now_peer_info_t peer = {
                    .ifidx = WIFI_IF_STA,
                    .encrypt = false};
                memcpy(peer.peer_addr, m.src, ESP_NOW_ETH_ALEN);
                esp_now_add_peer(&peer);
            }

            vTaskDelay(pdMS_TO_TICKS(500)); // Espero 500 ms antes de enviar
            // Enviar la configuración (estándar o específica) a la esfera
            intentar_enviar_configuracion_a_esfera(mac_str, m.src);

            // Muy importante: NO pasar este mensaje a esfera_manager_add()
            continue;
        }

        // 2) Datos → agrega al buffer
        esfera_manager_add(m.payload, mac_str);
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
    if (client == NULL)
    {
        ESP_LOGE(TAG, "❌ No se pudo inicializar el cliente MQTT");
        return;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));

    if (!s_espnow_rx_q)
    {
        s_espnow_rx_q = xQueueCreate(64, sizeof(espnow_rx_msg_t));
        xTaskCreatePinnedToCore(espnow_worker_task, "espnow_worker", 4096, NULL, 15, NULL, tskNO_AFFINITY);
    }
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
    if (msg_id != -1)
    {
        ESP_LOGI(TAG, "✅ Publicado a %s: %s", topic_public, payload);
    }
    else
    {
        ESP_LOGE(TAG, "❌ Error al publicar en el broker MQTT");
    }
}

void mqtt_manager_suscribirse(char *topic)
{
    if (client)
    {
        ESP_LOGI(TAG, "🧭 Suscribiéndose al topic: %s", topic);
        int msj_id = esp_mqtt_client_subscribe(client, topic, 1);
        if (msj_id < 0)
        {
            ESP_LOGE(TAG, "❌ Falló la suscripción al topic: %d", msj_id);
        }
        else
        {
            ESP_LOGI(TAG, "📥 Suscripción exitosa con msg_id: %d", msj_id);
            hub_iniciar_espnow();
        }
    }
    else
    {
        ESP_LOGE(TAG, "❌ Cliente MQTT no inicializado");
    }
}

esp_mqtt_client_handle_t mqtt_manager_obtener_cliente(void)
{
    return client;
}
