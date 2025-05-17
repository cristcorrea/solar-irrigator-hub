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

extern char mac_local[13];  // Formato XX:XX:XX:XX:XX:XX

//static char *topic_sus = "ismart/hub";
static esp_mqtt_client_handle_t client = NULL;

extern const uint8_t hivemq_certificate_pem_start[] asm("_binary_hivemq_certificate_pem_start");
// extern const uint8_t hivemq_certificate_pem_end[] asm("_binary_hivemq_certificate_pem_end");

extern const uint8_t ca_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t ca_cert_pem_end[] asm("_binary_ca_cert_pem_end");

extern const uint8_t client_cert_pem_start[] asm("_binary_client_cert_pem_start");
extern const uint8_t client_cert_pem_end[] asm("_binary_client_cert_pem_end");

extern const uint8_t client_key_pem_start[] asm("_binary_client_key_pem_start");
extern const uint8_t client_key_pem_end[] asm("_binary_client_key_pem_end");

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event);

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    mqtt_event_handler_cb(event);
}

static void intentar_enviar_configuracion_a_esfera(const char *mac_str, const uint8_t *mac_bin)
{
    char clave_nvs[20];
    
    char mac_clean[13]; // 12 + null terminator
    int j = 0;
    for (int i = 0; mac_str[i] && j < 12; i++)
    {
        if (mac_str[i] != ':')
        {
            mac_clean[j++] = mac_str[i];
        }
    }
    mac_clean[j] = '\0';
    snprintf(clave_nvs, sizeof(clave_nvs), "%s", mac_clean);
    // snprintf(clave_nvs, sizeof(clave_nvs), "%s", mac_clean);
    ESP_LOGI(TAG, "Clave buscada: %s", mac_clean);

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("config_store", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW("HUB", "No se pudo abrir NVS para leer configuración");
        return;
    }

    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, clave_nvs, NULL, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI("HUB", "No hay configuración pendiente para %s", clave_nvs);
        nvs_close(nvs_handle);
        return;
    }

    char *config_json = malloc(required_size);
    if (!config_json) {
        ESP_LOGE("HUB", "Error al asignar memoria para JSON");
        nvs_close(nvs_handle);
        return;
    }

    err = nvs_get_str(nvs_handle, clave_nvs, config_json, &required_size);
    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE("HUB", "No se pudo leer configuración para %s", mac_str);
        free(config_json);
        return;
    }

    // Enviar la configuración por ESP-NOW
    esp_err_t send_result = esp_now_send(mac_bin, (uint8_t *)config_json, strlen(config_json));
    if (send_result == ESP_OK) {
        ESP_LOGI("HUB", "Configuración enviada a %s", mac_str);
        // Nota: eliminar solo tras ACK de la esfera
    } else {
        ESP_LOGE("HUB", "Fallo al enviar configuración a %s", mac_str);
    }

    free(config_json);
}

static void procesar_configuracion_esfera(const char* payload)
{
    cJSON *json = cJSON_Parse(payload);
    if (!json) {
        ESP_LOGE(TAG, "Error al parsear JSON");
        return;
    }

    cJSON *data_flag = cJSON_GetObjectItem(json, "Data");
    if (!cJSON_IsBool(data_flag)) {
        ESP_LOGW(TAG, "Campo 'Data' inválido o ausente");
        cJSON_Delete(json);
        return;
    }

    if (cJSON_IsTrue(data_flag)) {
        // Se requiere publicar los datos almacenados (ya lo manejás)
        cJSON_Delete(json);
        return;
    }

    cJSON *mac_slave = cJSON_GetObjectItem(json, "MACSLAVE");
    if (!cJSON_IsString(mac_slave)) {
        ESP_LOGE(TAG, "MACSLAVE inválido o ausente");
        cJSON_Delete(json);
        return;
    }

    char *json_string = cJSON_PrintUnformatted(json);
    if (!json_string) {
        ESP_LOGE(TAG, "No se pudo convertir JSON a string");
        cJSON_Delete(json);
        return;
    }

    char clave_nvs[20];
    char mac_clean[13]; // 12 + null terminator
    int j = 0;
    for (int i = 0; mac_slave->valuestring[i] && j < 12; i++)
    {
        if (mac_slave->valuestring[i] != ':')
        {
            mac_clean[j++] = mac_slave->valuestring[i];
        }
    }
    mac_clean[j] = '\0';

    snprintf(clave_nvs, sizeof(clave_nvs), "%s", mac_clean);
    ESP_LOGI(TAG, "Clave %s", mac_clean);


    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("config_store", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error abriendo NVS: %s", esp_err_to_name(err));
        free(json_string);
        cJSON_Delete(json);
        return;
    }

    err = nvs_set_str(nvs_handle, clave_nvs, json_string);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
        ESP_LOGI(TAG, "Configuración almacenada para %s", mac_slave->valuestring);
    } else {
        ESP_LOGE(TAG, "Error escribiendo en NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    free(json_string);
    cJSON_Delete(json);
}

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "🔌 Conectado al broker MQTT");
        vTaskDelay(pdMS_TO_TICKS(500));
        mqtt_manager_suscribirse(topic_suscripcion);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "⚠️ Desconectado del broker MQTT");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "📡 Suscripción exitosa (msg_id=%d)", event->msg_id);
        hub_iniciar_espnow();
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "❎ Desuscripción (msg_id=%d)", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "📤 Publicado (msg_id=%d)", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
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

        // Verificar MACHUB
        // cJSON *machub = cJSON_GetObjectItem(json, "MACHUB");
        // if (!cJSON_IsString(machub))
        // {
        //     ESP_LOGW(TAG, "❌ MACHUB faltante o inválido");
        //     cJSON_Delete(json);
        //     break;
        // }

        // if (strcmp(machub->valuestring, mac_local) != 0)
        // {
        //     ESP_LOGI(TAG, "📵 Mensaje no destinado a este hub (%s)", mac_local);
        //     cJSON_Delete(json);
        //     break;
        // }

        // Revisar si es una petición de datos
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
        {   // Almacenar configuración de esfera
            ESP_LOGI(TAG, "⚙️ Configuración recibida (a implementar)");
            procesar_configuracion_esfera(payload);
        }

        cJSON_Delete(json);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "❌ Error en la conexión MQTT");
        break;

    default:
        ESP_LOGW(TAG, "🔄 Otro evento MQTT id: %d", event->event_id);
        break;
    }
    return ESP_OK;
}

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

    esp_now_peer_info_t peer = {
        .channel = 6,
        .ifidx = WIFI_IF_STA,
        .encrypt = false
    };
    memcpy(peer.peer_addr, recv_info->src_addr, ESP_NOW_ETH_ALEN);
    
    if (!esp_now_is_peer_exist(recv_info->src_addr)) {
        esp_now_add_peer(&peer);
    }else{
        intentar_enviar_configuracion_a_esfera(mac_str, recv_info->src_addr); 
    }

}

void mqtt_manager_init(void)
{

    // Obtener la MAC local en formato string
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_local, sizeof(mac_local), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "🆔 MAC local: %s", mac_local);

    // Construir topic de suscripción: ismart/hub/XX:XX:...
    snprintf(topic_public, sizeof(topic_public), "ismart/app/%s", mac_local);
    snprintf(topic_suscripcion, sizeof(topic_suscripcion), "ismart/hub/%s", mac_local);
    ESP_LOGI(TAG, "📡 Topic suscripción: %s", topic_suscripcion);
    ESP_LOGI(TAG, "📡 Topic publicación: %s", topic_public);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = MQTT_URI,
            .verification.certificate = (const char *)ca_cert_pem_start,
            .verification.use_global_ca_store = false,
            .verification.skip_cert_common_name_check = false,
        },
        .credentials = {
            .username = MQTT_USERNAME,
            .client_id = mac_local,
            .authentication.password = MQTT_PASSWORD, 
            .authentication.certificate = (const char *)client_cert_pem_start,
            .authentication.key = (const char *)client_key_pem_start
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

void mqtt_manager_handle_json_config(const char *payload)
{
    cJSON *json = cJSON_Parse(payload);
    if (!json)
    {
        ESP_LOGE("MQTT", "Error al parsear JSON");
        return;
    }

    const cJSON *machub = cJSON_GetObjectItem(json, "MACHUB");
    const cJSON *macslave = cJSON_GetObjectItem(json, "MACSLAVE");
    const cJSON *colorLED = cJSON_GetObjectItem(json, "colorLED");
    const cJSON *riegoAuto = cJSON_GetObjectItem(json, "riegoAuto");
    const cJSON *diasRiego = cJSON_GetObjectItem(json, "diasRiego");
    const cJSON *horaRiego = cJSON_GetObjectItem(json, "horaRiego");
    const cJSON *ml = cJSON_GetObjectItem(json, "ml");

    if (!machub || !macslave || !colorLED || !riegoAuto || !diasRiego || !horaRiego || !ml)
    {
        ESP_LOGE("MQTT", "Faltan campos en el JSON");
        cJSON_Delete(json);
        return;
    }

    // Abrir NVS
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("config", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE("NVS", "Error abriendo NVS (%s)", esp_err_to_name(err));
        cJSON_Delete(json);
        return;
    }

    // Guardar los valores
    nvs_set_str(nvs_handle, "machub", machub->valuestring);
    nvs_set_str(nvs_handle, "macslave", macslave->valuestring);

    uint32_t color = (uint32_t)colorLED->valuedouble; // usar valuedouble para evitar overflow con valueint
    nvs_set_u32(nvs_handle, "colorLED", color);

    nvs_set_u8(nvs_handle, "riegoAuto", (uint8_t)riegoAuto->valueint);
    nvs_set_u8(nvs_handle, "diasRiego", (uint8_t)diasRiego->valueint);
    nvs_set_str(nvs_handle, "horaRiego", horaRiego->valuestring);
    nvs_set_u16(nvs_handle, "ml", (uint16_t)ml->valueint);

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    cJSON_Delete(json);

    ESP_LOGI("MQTT", "Configuración guardada en NVS");

    // DEBUG opcional: extraer y mostrar RGB
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    ESP_LOGI("MQTT", "Color LED: R=%d G=%d B=%d", r, g, b);
}
