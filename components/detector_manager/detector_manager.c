#include "detector_manager.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define DEV_DETEC_GPIO  GPIO_NUM_5  // Cambiar si se conecta a otro pin
#define PS_ENB_GPIO     GPIO_NUM_4  // Cambiar si se conecta a otro pin

static const char *TAG = "detector_manager";

static void detector_task(void *arg)
{
    while (1) {
        int level = gpio_get_level(DEV_DETEC_GPIO);
        gpio_set_level(PS_ENB_GPIO, level == 0 ? 1 : 0);
        ESP_LOGI(TAG, "Level readed: %i", level);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void detector_manager_init(void)
{
    ESP_LOGI(TAG, "Inicializando detector de esfera");

    // Configurar DEV_DETEC como entrada
    gpio_config_t dev_detec_conf = {
        .pin_bit_mask = 1ULL << DEV_DETEC_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&dev_detec_conf);

    // Configurar PS_ENB como salida
    gpio_config_t ps_enb_conf = {
        .pin_bit_mask = 1ULL << PS_ENB_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&ps_enb_conf);
    gpio_set_level(PS_ENB_GPIO, 0);  // Apagar al inicio

    // Crear la tarea de detección
    xTaskCreate(detector_task, "detector_task", 2048, NULL, 5, NULL);
}
