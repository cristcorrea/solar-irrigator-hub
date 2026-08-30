#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <netdb.h>
#include "lwip/dns.h"
#include "mqtt_manager.h"
#include "blufi_manager.h"
#include <inttypes.h>  
#include "cJSON.h"
#include "esfera_manager.h"
#include "time_sync.h"
#include "detector_manager.h"
#include "button_manager.h"
#include "app_mode_manager.h"


#define TAG "HUB"

// float humedad, temperatura, voltaje;
// int riego;
// char mac_dato[32];

void hub_enviar_broadcast_descubrimiento(void);

SemaphoreHandle_t semaforo_wifi_listo;
SemaphoreHandle_t semaforo_time_listo; 

static bool s_espnow_iniciado = false;   
static bool s_online_services_started = false;

char mac_local[13] = {0};  // Formato XX:XX:XX:XX:XX:XX

static void hub_init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

esp_err_t hub_iniciar_espnow(void)
{
    ESP_LOGI("HUB", "⚙️ Iniciando ESP-NOW...");

    if (s_espnow_iniciado)
    {
        ESP_LOGW("HUB", "ESP-NOW ya estaba iniciado");
        return ESP_OK;
    }

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar ESP-NOW: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo desactivar power-save Wi-Fi: %s", esp_err_to_name(err));
    }

    err = esp_now_register_recv_cb(espnow_recv_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo registrar callback ESP-NOW: %s", esp_err_to_name(err));
        esp_now_deinit();
        return err;
    }

    esp_now_peer_info_t broadcast_peer = {
        .ifidx = WIFI_IF_STA,
        .encrypt = false};

    memset(broadcast_peer.peer_addr, 0xFF, ESP_NOW_ETH_ALEN);
    err = esp_now_add_peer(&broadcast_peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "No se pudo registrar peer broadcast: %s", esp_err_to_name(err));
        esp_now_deinit();
        return err;
    }

    s_espnow_iniciado = true;
    return ESP_OK;
}

// En hub_station.c

void hub_enviar_broadcast_descubrimiento(void)
{
    if (!s_espnow_iniciado) {
        ESP_LOGW(TAG, "ESP-NOW no iniciado: no se envía broadcast");
        return;
    }

    uint8_t bcast_addr[ESP_NOW_ETH_ALEN];
    memset(bcast_addr, 0xFF, sizeof(bcast_addr));

    char payload[64];
    int len = snprintf(payload, sizeof(payload),
                       "HELLO_ESFERA,%s", mac_local);
    if (len <= 0) {
        ESP_LOGE(TAG, "Error formateando payload de broadcast");
        return;
    }

    esp_err_t err = esp_now_send(bcast_addr, (uint8_t *)payload, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error enviando broadcast ESP-NOW: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Broadcast ESP-NOW enviado: %s", payload);
    }
}

bool hub_aceptar_respuesta_esfera(void)
{
    return detector_manager_accept_esfera_response();
}

static bool hub_wifi_is_connected(void)
{
    wifi_ap_record_t ap_info;
    return esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
}

static void hub_start_online_services(void)
{
    if (s_online_services_started) {
        return;
    }

    ESP_LOGI(TAG, "Modo online: iniciando SNTP/MQTT");

    time_sync_init("CET-1CEST,M3.5.0/2,M10.5.0/3");

    if (time_sync_is_synchronized() ||
        xSemaphoreTake(semaforo_time_listo, pdMS_TO_TICKS(15000)) == pdTRUE) {
        struct tm now = time_sync_get_time();
        ESP_LOGI("MAIN", "Hora actual: %02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);
    } else {
        ESP_LOGW(TAG, "SNTP no respondió en 15 s; MQTT reintentará mientras llega la hora");
    }

    ESP_LOGI(TAG, "Iniciando protocolo MQTT");

    esp_err_t err = mqtt_manager_init();
    if (err == ESP_OK) {
        s_online_services_started = true;
    } else {
        ESP_LOGE(TAG, "No se pudo iniciar MQTT: %s", esp_err_to_name(err));
    }
}

static void hub_mode_services_task(void *arg)
{
    EventGroupHandle_t mode_events = app_mode_manager_event_group();
    bool online_wait_logged = false;
    bool offline_started_warned = false;

    for (;;) {
        app_mode_t mode = APP_MODE_OFFLINE;
        ESP_ERROR_CHECK_WITHOUT_ABORT(app_mode_manager_get(&mode));

        if (mode == APP_MODE_ONLINE) {
            offline_started_warned = false;
            if (!s_online_services_started && hub_wifi_is_connected()) {
                hub_start_online_services();
            } else if (!s_online_services_started && !online_wait_logged) {
                ESP_LOGI(TAG, "Modo online: esperando configuración/conexión Wi-Fi por BLUFI");
                online_wait_logged = true;
            }
        } else {
            online_wait_logged = false;
            if (hub_wifi_is_connected()) {
                ESP_LOGI(TAG, "Modo offline: desconectando Wi-Fi");
                esp_wifi_disconnect();
            }

            if (!s_espnow_iniciado) {
                ESP_LOGI(TAG, "Modo offline: servicios online MQTT/SNTP no se inician");
                ESP_ERROR_CHECK_WITHOUT_ABORT(hub_iniciar_espnow());
            }

            if (s_online_services_started) {
                if (mqtt_manager_stop() == ESP_OK) {
                    s_online_services_started = false;
                }
                if (!offline_started_warned) {
                    ESP_LOGI(TAG, "Modo offline activo: MQTT detenido");
                    offline_started_warned = true;
                }
            }
        }

        if (mode_events != NULL) {
            xEventGroupWaitBits(mode_events,
                                APP_MODE_EVENT_ONLINE_REQUESTED | APP_MODE_EVENT_OFFLINE_REQUESTED,
                                pdTRUE,
                                pdFALSE,
                                pdMS_TO_TICKS(1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}


void app_main(void)
{
    ESP_LOGI(TAG, "[HUB] Iniciando...");

    hub_init_nvs();
    ESP_ERROR_CHECK(app_mode_manager_init());
    ESP_ERROR_CHECK(app_mode_manager_set(APP_MODE_ONLINE));
    ESP_ERROR_CHECK(esfera_manager_init());

    app_mode_t app_mode = APP_MODE_OFFLINE;
    ESP_ERROR_CHECK(app_mode_manager_get(&app_mode));
    ESP_LOGI(TAG, "Modo de trabajo: %s", app_mode_manager_to_string(app_mode));

    button_init();

        // Obtener la MAC local en formato string
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_local, sizeof(mac_local), "%02X%02X%02X%02X%02X%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "🆔 MAC local: %s", mac_local);

    semaforo_wifi_listo = xSemaphoreCreateBinary();
    semaforo_time_listo = xSemaphoreCreateBinary(); 
    if (semaforo_wifi_listo == NULL || semaforo_time_listo == NULL) {
        ESP_LOGE(TAG, "No se pudieron crear los semáforos de arranque");
        return;
    }



    ESP_LOGI(TAG, "📲 Iniciando BLUFI...");
    ESP_ERROR_CHECK(blufi_init());
    ESP_ERROR_CHECK(mqtt_manager_init_espnow_processing());
    ESP_ERROR_CHECK(hub_iniciar_espnow());

    BaseType_t task_created = xTaskCreate(hub_mode_services_task, "hub_mode_services", 4096, NULL, 6, NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear la tarea de gestión de modo");
        return;
    }

    ESP_LOGI(TAG, "📡 HUB listo para recibir datos por ESP-NOW...");

    detector_manager_init();

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
