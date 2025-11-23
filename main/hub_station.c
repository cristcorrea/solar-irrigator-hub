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


#define TAG "HUB"

// float humedad, temperatura, voltaje;
// int riego;
// char mac_dato[32];

void hub_enviar_broadcast_descubrimiento(void);

SemaphoreHandle_t semaforo_wifi_listo;
SemaphoreHandle_t semaforo_time_listo; 

static bool s_espnow_iniciado = false;   

char mac_local[13] = {0};  // Formato XX:XX:XX:XX:XX:XX

void hub_iniciar_espnow(void)
{
    ESP_LOGI("HUB", "⚙️ Iniciando ESP-NOW...");

    if (s_espnow_iniciado)
    {
        ESP_LOGW("HUB", "ESP-NOW ya estaba iniciado");
        return;
    }

    ESP_ERROR_CHECK(esp_now_init());
    esp_wifi_set_ps(WIFI_PS_NONE); 
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    esp_now_peer_info_t broadcast_peer = {
        .ifidx = WIFI_IF_STA,
        .encrypt = false};

    memset(broadcast_peer.peer_addr, 0xFF, ESP_NOW_ETH_ALEN);
    esp_now_add_peer(&broadcast_peer);

    s_espnow_iniciado = true;   // <<< NUEVO

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


void app_main(void)
{
    ESP_LOGI(TAG, "[HUB] Iniciando...");

    button_init();

        // Obtener la MAC local en formato string
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_local, sizeof(mac_local), "%02X%02X%02X%02X%02X%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "🆔 MAC local: %s", mac_local);

    semaforo_wifi_listo = xSemaphoreCreateBinary();
    semaforo_time_listo = xSemaphoreCreateBinary(); 



    ESP_LOGI(TAG, "📲 Iniciando BLUFI...");
    blufi_init();

    ESP_LOGI(TAG, "⏳ Esperando configuración Wi-Fi por BLUFI...");
    //Espera a que finalice la conexion Wi-Fi para sincronizar la hora. 
    xSemaphoreTake(semaforo_wifi_listo, portMAX_DELAY);

    time_sync_init("CET-1CEST,M3.5.0/2,M10.5.0/3");

    vTaskDelay(pdMS_TO_TICKS(500));

    //Espera a que finalice la sincronizacion de la hora para consultar. 
    if(xSemaphoreTake(semaforo_time_listo, portMAX_DELAY) == pdTRUE)
    {
        struct tm now = time_sync_get_time();
        ESP_LOGI("MAIN", "Hora actual: %02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);
    }

    ESP_LOGI("TAG", "Iniciando protocolo MQTT"); 

    vTaskDelay(pdMS_TO_TICKS(1000));

    mqtt_manager_init();

    ESP_LOGI(TAG, "📡 HUB listo para recibir datos por ESP-NOW...");

    detector_manager_init();

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
