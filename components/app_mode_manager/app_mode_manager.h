#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define APP_MODE_EVENT_ONLINE_REQUESTED  BIT0
#define APP_MODE_EVENT_OFFLINE_REQUESTED BIT1

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_MODE_OFFLINE = 0,
    APP_MODE_ONLINE = 1,
} app_mode_t;

esp_err_t app_mode_manager_init(void);
esp_err_t app_mode_manager_get(app_mode_t *mode);
esp_err_t app_mode_manager_set(app_mode_t mode);
void app_mode_manager_apply_committed(app_mode_t mode);

const char *app_mode_manager_to_string(app_mode_t mode);
esp_err_t app_mode_manager_parse(const char *value, app_mode_t *mode);
EventGroupHandle_t app_mode_manager_event_group(void);

#ifdef __cplusplus
}
#endif
