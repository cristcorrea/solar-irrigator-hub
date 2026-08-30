#include "detector_manager.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "hub_station.h"
#include <stdatomic.h>



#define DEV_DETEC_GPIO  GPIO_NUM_5  
#define PS_ENB_GPIO     GPIO_NUM_4  
#define DISCOVERY_START_DELAY_MS 100
#define DISCOVERY_RETRY_MS       500
#define DISCOVERY_TIMEOUT_MS     20000

static const char *TAG = "detector_manager";
static TaskHandle_t detector_task_handle;
static atomic_bool discovery_active = ATOMIC_VAR_INIT(false);

static void detector_task(void *arg)
{
    static int last_level = 1;

    while (1) {
        int current_level = gpio_get_level(DEV_DETEC_GPIO);
        if (last_level == 1 && current_level == 0){
            gpio_set_level(PS_ENB_GPIO, 1);
            ESP_LOGI(TAG, "Esfera detectada con éxito!");
            // Pequeño retardo para que la esfera empiece a arrancar
            ulTaskNotifyTake(pdTRUE, 0);
            vTaskDelay(pdMS_TO_TICKS(DISCOVERY_START_DELAY_MS));

            // Abrir una ventana de descubrimiento limitada a 20 segundos.
            TickType_t discovery_start = xTaskGetTickCount();
            atomic_store(&discovery_active, true);
            while (gpio_get_level(DEV_DETEC_GPIO) == 0 &&
                   (xTaskGetTickCount() - discovery_start) < pdMS_TO_TICKS(DISCOVERY_TIMEOUT_MS)) {
                hub_enviar_broadcast_descubrimiento();
                if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DISCOVERY_RETRY_MS)) > 0) {
                    ESP_LOGI(TAG, "Respuesta recibida; fin del descubrimiento");
                    break;
                }
            }
            bool was_active = atomic_exchange(&discovery_active, false);
            if (was_active && gpio_get_level(DEV_DETEC_GPIO) == 0) {
                ESP_LOGW(TAG, "Tiempo de descubrimiento agotado tras %d segundos",
                         DISCOVERY_TIMEOUT_MS / 1000);
            }
        } else if (last_level == 0 && current_level == 1) {
            gpio_set_level(PS_ENB_GPIO, 0);
            ESP_LOGI(TAG, "Esfera desconectada");
        }
        last_level = gpio_get_level(DEV_DETEC_GPIO);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool detector_manager_accept_esfera_response(void)
{
    if (!atomic_exchange(&discovery_active, false)) {
        return false;
    }

    if (detector_task_handle != NULL) {
        xTaskNotifyGive(detector_task_handle);
    }
    return true;
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
    BaseType_t created = xTaskCreate(detector_task, "detector_task", 4096, NULL, 5,
                                     &detector_task_handle);
    if (created != pdPASS) {
        detector_task_handle = NULL;
        ESP_LOGE(TAG, "No se pudo crear la tarea de deteccion");
    }
}
