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
#include <math.h>

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
static int s_pending_telemetry_msg_id = -1;
static uint32_t s_pending_page_id = 0;
static uint32_t s_pending_last_seq = 0;

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
static void intentar_enviar_configuracion_a_esfera(const char *mac_str, const uint8_t *mac_bin,
                                                   bool force_send);
static void procesar_mensaje_ctrl_esfera(const char *mac_str, const char *payload);

// Helper: convierte "A085E369D6AC" -> uint8_t[6]
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool normalize_mac(const char *input, char output[13])
{
    if (input == NULL || output == NULL) {
        return false;
    }

    size_t out_index = 0;
    for (size_t i = 0; input[i] != '\0'; ++i) {
        if (input[i] == ':') {
            continue;
        }
        if (out_index >= 12 || hex_nibble(input[i]) < 0) {
            return false;
        }
        char value = input[i];
        output[out_index++] = (value >= 'a' && value <= 'f') ? (char)(value - 'a' + 'A') : value;
    }
    output[out_index] = '\0';
    return out_index == 12;
}

static bool mac_str_to_bytes(const char *mac_str, uint8_t out[6])
{
    char normalized[13];
    if (!normalize_mac(mac_str, normalized)) return false;

    for (int i = 0; i < 6; i++) {
        int hi = hex_nibble(normalized[2 * i]);
        int lo = hex_nibble(normalized[2 * i + 1]);
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
    snprintf(key, sizeof(key), "a%s", mac_clean);
    esp_err_t err = nvs_get_u8(h, key, &ack);
    nvs_close(h);
    return (err == ESP_OK && ack == 1);
}

static esp_err_t cfg_ack_set_applied(const char *mac_clean, bool applied)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("config_store", NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    char key[20];
    snprintf(key, sizeof(key), "a%s", mac_clean);
    err = nvs_set_u8(h, key, applied ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

// ============================================================
//   ENVÍO DE CONFIGURACIÓN A UNA ESFERA
// ============================================================
static void intentar_enviar_configuracion_a_esfera(const char *mac_str, const uint8_t *mac_bin,
                                                   bool force_send)
{

    esp_err_t send_result;

    char mac_clean[13];
    if (!normalize_mac(mac_str, mac_clean)) {
        ESP_LOGE(TAG, "MAC inválida al enviar configuración");
        return;
    }

    // --- NUEVO: no enviar si ya aplicada ---
    if (!force_send && cfg_ack_is_applied(mac_clean))
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

        send_result = esp_now_send(mac_bin, (uint8_t *)buffer, strlen(buffer));
        if (send_result == ESP_OK) {
            ESP_LOGI(TAG, "✅ Configuración estándar enviada a %s", mac_str);
        } else {
            ESP_LOGE(TAG, "No se pudo enviar configuración estándar: %s", esp_err_to_name(send_result));
        }
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

        send_result = esp_now_send(mac_bin, (uint8_t *)buffer, strlen(buffer));
        if (send_result == ESP_OK) {
            ESP_LOGI(TAG, "✅ Configuración estándar enviada a %s", mac_str);
        } else {
            ESP_LOGE(TAG, "No se pudo enviar configuración estándar: %s", esp_err_to_name(send_result));
        }
        nvs_close(nvs_handle);
        return;
    }
    if (err != ESP_OK || required_size == 0) {
        ESP_LOGE(TAG, "No se pudo consultar configuración de %s: %s", mac_str, esp_err_to_name(err));
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
    char mac_clean[13];
    esp_err_t err = esfera_manager_store_config(payload, strlen(payload), mac_clean);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "💾 Configuración almacenada para %s", mac_clean);
    } else {
        ESP_LOGE(TAG, "Configuración rechazada: %s", esp_err_to_name(err));
    }
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
        mqtt_manager_suscribirse(topic_suscripcion);
        break;

    case MQTT_EVENT_DATA:
    {
        if (event->topic_len != (int)strlen(topic_suscripcion) ||
            memcmp(event->topic, topic_suscripcion, event->topic_len) != 0) {
            ESP_LOGW(TAG, "Mensaje descartado por topic inesperado");
            break;
        }

        if (event->current_data_offset != 0 || event->data_len != event->total_data_len ||
            event->data_len <= 0 || event->data_len >= 256) {
            ESP_LOGW(TAG, "Payload MQTT fragmentado o fuera de límite: %d/%d bytes",
                     event->data_len, event->total_data_len);
            break;
        }

        ESP_LOGI(TAG, "📥 Mensaje recibido en %.*s (%d bytes)",
                 event->topic_len, event->topic, event->data_len);

        char payload[256];
        int len = event->data_len;
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

            if (s_pending_telemetry_msg_id >= 0) {
                ESP_LOGW(TAG, "Ya hay una entrega de telemetría pendiente de ACK MQTT");
                cJSON_Delete(json);
                break;
            }

            size_t max = 50;
            cJSON *max_item = cJSON_GetObjectItemCaseSensitive(json, "max");
            if (max_item != NULL) {
                if (!cJSON_IsNumber(max_item) || max_item->valuedouble < 1 ||
                    max_item->valuedouble > 100 || max_item->valuedouble != floor(max_item->valuedouble)) {
                    ESP_LOGW(TAG, "max de telemetría inválido");
                    cJSON_Delete(json);
                    break;
                }
                max = (size_t)max_item->valueint;
            }
            uint32_t page_id = 0, last_seq = 0;
            size_t pend = 0;
            char *json_out = esfera_manager_page_json(max, &page_id, &last_seq, &pend);
            if (json_out == NULL) {
                ESP_LOGE(TAG, "No se pudo generar la respuesta de telemetría");
                cJSON_Delete(json);
                break;
            }

            int msg_id = esp_mqtt_client_enqueue(event->client, topic_public, json_out, 0, 1, 0, true);
            free(json_out);
            if (msg_id < 0) {
                ESP_LOGE(TAG, "No se pudo encolar la telemetría MQTT; se conserva en NVS");
            } else {
                s_pending_telemetry_msg_id = msg_id;
                s_pending_page_id = page_id;
                s_pending_last_seq = last_seq;
            }
        }
        else
        {
            cJSON *cmd = cJSON_GetObjectItemCaseSensitive(json, "cmd");
#if CONFIG_TELEMETRY_LOG_TEST_MODE
            if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "debug_fill") == 0) {
                cJSON *hub = cJSON_GetObjectItemCaseSensitive(json, "MACHUB");
                cJSON *n = cJSON_GetObjectItemCaseSensitive(json, "n");
                bool valid = cJSON_IsString(hub) && strcmp(hub->valuestring, mac_local) == 0 &&
                             cJSON_IsNumber(n) && isfinite(n->valuedouble) &&
                             n->valuedouble >= 1 && n->valuedouble <= 3000 &&
                             n->valuedouble == floor(n->valuedouble);
                size_t written = 0;
                esp_err_t fill_err = valid
                                         ? esfera_manager_debug_fill((size_t)n->valueint, &written)
                                         : ESP_ERR_INVALID_ARG;
                char response[112];
                snprintf(response, sizeof(response),
                         "{\"cmd\":\"debug_fill\",\"status\":\"%s\",\"written\":%u}",
                         fill_err == ESP_OK ? "ok" : "error", (unsigned)written);
                esp_mqtt_client_publish(event->client, topic_public, response, 0, 1, 0);
            } else
#endif
            if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "ack_data") == 0) {
                cJSON *hub = cJSON_GetObjectItemCaseSensitive(json, "MACHUB");
                cJSON *page = cJSON_GetObjectItemCaseSensitive(json, "page_id");
                cJSON *seq = cJSON_GetObjectItemCaseSensitive(json, "last_seq");
                bool valid_ack = cJSON_IsString(hub) && strcmp(hub->valuestring, mac_local) == 0 &&
                                 cJSON_IsNumber(page) && cJSON_IsNumber(seq) &&
                                 page->valuedouble >= 0 && page->valuedouble <= UINT32_MAX &&
                                 seq->valuedouble >= 0 && seq->valuedouble <= UINT32_MAX &&
                                 page->valuedouble == floor(page->valuedouble) &&
                                 seq->valuedouble == floor(seq->valuedouble);
                if (!valid_ack) {
                    ESP_LOGW(TAG, "ack_data MQTT inválido o dirigido a otro hub");
                } else {
                    uint32_t page_id = (uint32_t)page->valuedouble;
                    uint32_t last_seq = (uint32_t)seq->valuedouble;
                    esp_err_t ack_err = esfera_manager_ack(page_id, last_seq);
                    if (ack_err == ESP_OK && page_id == s_pending_page_id &&
                        last_seq == s_pending_last_seq) {
                        s_pending_telemetry_msg_id = -1;
                        s_pending_page_id = 0;
                        s_pending_last_seq = 0;
                    }
                    char response[96];
                    snprintf(response, sizeof(response),
                             "{\"cmd\":\"ack_data\",\"status\":\"%s\",\"pend\":%u}",
                             ack_err == ESP_OK ? "ok" : "error",
                             (unsigned)esfera_manager_count());
                    esp_mqtt_client_publish(event->client, topic_public, response, 0, 1, 0);
                }
            } else {
                ESP_LOGI(TAG, "Configuración recibida");
                procesar_configuracion_esfera(payload);
            }
        }

        cJSON_Delete(json);
        break;
    }

    case MQTT_EVENT_PUBLISHED:
        if (event->msg_id == s_pending_telemetry_msg_id) {
            ESP_LOGI(TAG, "PUBACK de página MQTT recibido; esperando ack_data de la app");
            s_pending_telemetry_msg_id = -1;
        }
        break;

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
        esp_err_t ack_err = cfg_ack_set_applied(mac_clean, true);
        if (ack_err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo persistir ACK de configuración: %s", esp_err_to_name(ack_err));
        }

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
                esp_err_t peer_err = esp_now_add_peer(&peer);
                if (peer_err != ESP_OK && peer_err != ESP_ERR_ESPNOW_EXIST) {
                    ESP_LOGW(TAG, "No se pudo registrar peer para ACK: %s", esp_err_to_name(peer_err));
                    return;
                }
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
    if (!s_espnow_rx_q || recv_info == NULL || recv_info->src_addr == NULL || data == NULL ||
        len <= 0 || len >= (int)sizeof(((espnow_rx_msg_t *)0)->payload)) {
        return;
    }

    espnow_rx_msg_t m = (espnow_rx_msg_t){0};
    memcpy(m.src, recv_info->src_addr, ESP_NOW_ETH_ALEN);
    m.len = (uint16_t)len;
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

        ESP_LOGI(TAG, "Mensaje ESP-NOW recibido de %s: '%s'", mac_str, m.payload);

        // 1) Control de config (CFG_OK / CFG_ERR) para no romper el parser de datos
        if (strncmp(m.payload, "CFG_", 4) == 0)
        {
            if (!esfera_manager_is_registered(mac_str)) {
                ESP_LOGW(TAG, "Control rechazado de dispositivo no registrado: %s", mac_str);
                continue;
            }
            procesar_mensaje_ctrl_esfera(mac_str, m.payload);
            continue;
        }

        // 1.b) Mensaje de emparejamiento: HELLO_HUB,<MAC_ESFERA>
        if (strncmp(m.payload, "HELLO_HUB,", 10) == 0)
        {
            char claimed_mac[13];
            if (!normalize_mac(m.payload + 10, claimed_mac) || strcmp(claimed_mac, mac_str) != 0) {
                ESP_LOGW(TAG, "HELLO_HUB rechazado: MAC declarada no coincide con remitente %s", mac_str);
                continue;
            }

            if (!hub_aceptar_respuesta_esfera()) {
                ESP_LOGW(TAG, "HELLO_HUB rechazado fuera de la ventana de emparejamiento: %s",
                         mac_str);
                continue;
            }

            ESP_LOGI(TAG, "🤝 HELLO_HUB recibido de %s: %s", mac_str, m.payload);

            // Registrar la esfera en NVS (si no existía)
            esp_err_t register_err = esfera_manager_register_mac(mac_str);
            if (register_err != ESP_OK) {
                ESP_LOGE(TAG, "No se pudo registrar %s: %s", mac_str, esp_err_to_name(register_err));
                continue;
            }

            // Asegurar peer ESP-NOW
            if (!esp_now_is_peer_exist(m.src))
            {
                esp_now_peer_info_t peer = {
                    .ifidx = WIFI_IF_STA,
                    .encrypt = false};
                memcpy(peer.peer_addr, m.src, ESP_NOW_ETH_ALEN);
                esp_err_t peer_err = esp_now_add_peer(&peer);
                if (peer_err != ESP_OK && peer_err != ESP_ERR_ESPNOW_EXIST) {
                    ESP_LOGE(TAG, "No se pudo registrar peer %s: %s", mac_str, esp_err_to_name(peer_err));
                    continue;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(500)); // Espero 500 ms antes de enviar
            // Enviar la configuración (estándar o específica) a la esfera
            intentar_enviar_configuracion_a_esfera(mac_str, m.src, false);

            // Muy importante: NO pasar este mensaje a esfera_manager_add()
            continue;
        }

        // 2) Datos → agrega al buffer
        if (!esfera_manager_is_registered(mac_str)) {
            ESP_LOGW(TAG, "Telemetría rechazada de dispositivo no registrado: %s", mac_str);
            continue;
        }
        esp_err_t add_err = esfera_manager_add(m.payload, mac_str);
        if (add_err != ESP_OK) {
            ESP_LOGW(TAG, "Telemetría de %s descartada: %s", mac_str, esp_err_to_name(add_err));
            continue;
        }

        if (!esp_now_is_peer_exist(m.src)) {
            esp_now_peer_info_t peer = {
                .ifidx = WIFI_IF_STA,
                .encrypt = false,
            };
            memcpy(peer.peer_addr, m.src, ESP_NOW_ETH_ALEN);
            esp_err_t peer_err = esp_now_add_peer(&peer);
            if (peer_err != ESP_OK && peer_err != ESP_ERR_ESPNOW_EXIST) {
                ESP_LOGE(TAG, "No se pudo registrar peer %s para enviar configuración: %s",
                         mac_str, esp_err_to_name(peer_err));
                continue;
            }
        }

        ESP_LOGI(TAG, "Telemetría encolada; enviando configuración a %s", mac_str);
        intentar_enviar_configuracion_a_esfera(mac_str, m.src, true);
    }
}

// ============================================================
//   INICIALIZACIÓN Y PUBLICACIÓN MQTT
// ============================================================
esp_err_t mqtt_manager_init_espnow_processing(void)
{
    if (s_espnow_rx_q != NULL) {
        return ESP_OK;
    }

    s_espnow_rx_q = xQueueCreate(64, sizeof(espnow_rx_msg_t));
    if (s_espnow_rx_q == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreatePinnedToCore(espnow_worker_task, "espnow_worker", 4096,
                                                 NULL, 15, NULL, tskNO_AFFINITY);
    if (created != pdPASS) {
        vQueueDelete(s_espnow_rx_q);
        s_espnow_rx_q = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t mqtt_manager_init(void)
{
    if (client) {
        ESP_LOGW(TAG, "Cliente MQTT ya iniciado");
        return ESP_OK;
    }

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
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err == ESP_OK) {
        err = esp_mqtt_client_start(client);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo arrancar MQTT: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(client);
        client = NULL;
        return err;
    }

    ESP_LOGI(TAG, "✅ Cliente MQTT iniciado");
    return ESP_OK;
}

esp_err_t mqtt_manager_stop(void)
{
    if (!client) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deteniendo cliente MQTT");
    esp_err_t err = esp_mqtt_client_stop(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo detener MQTT: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_mqtt_client_destroy(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo destruir MQTT: %s", esp_err_to_name(err));
        return err;
    }
    client = NULL;
    s_pending_telemetry_msg_id = -1;
    s_pending_page_id = 0;
    s_pending_last_seq = 0;
    return ESP_OK;
}

void mqtt_manager_publicar_datos(const char *mac, float temperatura, float humedad, float voltaje, int riego)
{
    if (client == NULL) {
        ESP_LOGW(TAG, "MQTT no iniciado; no se publican datos");
        return;
    }
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
