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


#define TAG "HUB"

// float humedad, temperatura, voltaje;
// int riego;
// char mac_dato[32];

SemaphoreHandle_t semaforo_wifi_listo;
SemaphoreHandle_t semaforo_time_listo; 

char mac_local[18] = {0};  // Formato XX:XX:XX:XX:XX:XX

void hub_iniciar_espnow(void)
{
    ESP_LOGI("HUB", "⚙️ Iniciando ESP-NOW...");

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    esp_now_peer_info_t broadcast_peer = {
        .channel = 6,
        .ifidx = WIFI_IF_STA,
        .encrypt = false};

    memset(broadcast_peer.peer_addr, 0xFF, ESP_NOW_ETH_ALEN);
    esp_now_add_peer(&broadcast_peer);
}



void app_main(void)
{
    ESP_LOGI(TAG, "[HUB] Iniciando...");

        // Obtener la MAC local en formato string
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_local, sizeof(mac_local), "%02X%02X%02X%02X%02X%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "🆔 MAC local: %s", mac_local);

    semaforo_wifi_listo = xSemaphoreCreateBinary();
    semaforo_time_listo = xSemaphoreCreateBinary(); 

    nvs_flash_erase(); 

    ESP_LOGI(TAG, "📲 Iniciando BLUFI...");
    blufi_init();

    ESP_LOGI(TAG, "⏳ Esperando configuración Wi-Fi por BLUFI...");
    xSemaphoreTake(semaforo_wifi_listo, portMAX_DELAY);

    time_sync_init("CET-1CEST,M3.5.0/2,M10.5.0/3");

    vTaskDelay(pdMS_TO_TICKS(500));

    if(xSemaphoreTake(semaforo_time_listo, portMAX_DELAY) == pdTRUE)
    {
        struct tm now = time_sync_get_time();
        ESP_LOGI("MAIN", "Hora actual: %02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);
    }

    ESP_LOGI("TAG", "Iniciando protocolo MQTT"); 

    vTaskDelay(pdMS_TO_TICKS(1000));

    mqtt_manager_init();

    ESP_LOGI(TAG, "📡 HUB listo para recibir datos por ESP-NOW...");


    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
