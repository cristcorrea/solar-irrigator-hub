#include "button_manager.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nvs_flash.h"

#define BTN_GPIO                19
#define DEBOUNCE_US             40000      // 40 ms
#define LONG_PRESS_US           2000000    // 2 s
#define VERY_LONG_PRESS_US      5000000    // 5 s

static const char *TAG = "BTN";
static QueueHandle_t s_btn_evt_queue;
static esp_timer_handle_t s_debounce_tmr;
static volatile int s_last_level;          // 1=idle (pull-up), 0=pressed
static volatile int64_t s_last_edge_us;    // para debounce
static volatile int64_t s_press_start_us;  // timestamp de pulsación

typedef enum { BTN_EDGE_FALL = 0, BTN_EDGE_RISE = 1 } btn_edge_t;
typedef struct { btn_edge_t edge; int level; int64_t t_us; } btn_evt_t;

static void IRAM_ATTR gpio_isr(void *arg)
{
    int level = gpio_get_level(BTN_GPIO);
    int64_t now = esp_timer_get_time();

    // Debounce por tiempo mínimo entre edges
    if ((now - s_last_edge_us) < 1000) return;  // 1 ms guard band en ISR

    s_last_edge_us = now;
    s_last_level = level;

    // Notificamos a la task y disparamos un timer de debounce para confirmar estado estable
    btn_evt_t evt = { .edge = level ? BTN_EDGE_RISE : BTN_EDGE_FALL, .level = level, .t_us = now };
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_btn_evt_queue, &evt, &hpw);
    if (hpw == pdTRUE) portYIELD_FROM_ISR();

    esp_timer_stop(s_debounce_tmr);
    esp_timer_start_once(s_debounce_tmr, DEBOUNCE_US);
}

static void debounce_timer_cb(void *arg)
{
    // Relee estado tras debounce y manda evento de confirmación si cambió
    int level = gpio_get_level(BTN_GPIO);
    if (level != s_last_level) {
        s_last_level = level;
        btn_evt_t evt = { .edge = level ? BTN_EDGE_RISE : BTN_EDGE_FALL, .level = level, .t_us = esp_timer_get_time() };
        xQueueSend(s_btn_evt_queue, &evt, 0);
    }
}

static void button_task(void *arg)
{
    btn_evt_t evt;
    while (1) {
        if (xQueueReceive(s_btn_evt_queue, &evt, portMAX_DELAY) == pdTRUE) {
            if (evt.edge == BTN_EDGE_FALL) {
                // Botón presionado (va a 0)
                s_press_start_us = evt.t_us;
                ESP_LOGD(TAG, "press");
            } else { // BTN_EDGE_RISE
                // Botón liberado (vuelve a 1)
                int64_t dur = evt.t_us - s_press_start_us;
                ESP_LOGI(TAG, "release, dur=%.0f ms", dur/1000.0);

                if (dur >= VERY_LONG_PRESS_US) {
                    ESP_LOGW(TAG, "Very long press → ERASE NVS + restart");
                    // Acciones sensibles SIEMPRE fuera de ISR:
                    nvs_flash_erase();   // borrar todas las particiones NVS por defecto
                    nvs_flash_init();    // opcional, para dejar consistente
                    esp_restart();
                } else if (dur >= LONG_PRESS_US) {
                    ESP_LOGI(TAG, "Long press → acción larga (p.ej. entrar en modo setup)");
                    // TODO: tu acción (ej. marcar flag de modo configuración)
                } else {
                    ESP_LOGI(TAG, "Short press → acción corta (p.ej. enviar comando MQTT)");
                    // TODO: tu acción (ej. publicar 'S' por MQTT)
                }
            }
        }
    }
}

void button_init(void)
{
    // GPIO19: entrada con pull-up, interrupciones por ambos flancos
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    // Cola y timer de debounce
    s_btn_evt_queue = xQueueCreate(8, sizeof(btn_evt_t));
    const esp_timer_create_args_t targs = {
        .callback = debounce_timer_cb,
        .name = "btn_db"
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_debounce_tmr));

    // ISR
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_GPIO, gpio_isr, NULL));

    // Nivel inicial
    s_last_level = gpio_get_level(BTN_GPIO);

    // Task
    xTaskCreate(button_task, "button_task", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "Button on GPIO%d ready (pull-up, ANYEDGE, debounce %d ms)", BTN_GPIO, DEBOUNCE_US/1000);
}
