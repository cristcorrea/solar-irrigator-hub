#include "app_mode_manager.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define APP_MODE_NAMESPACE "hub_config"
#define APP_MODE_KEY       "work_mode"

static const char *TAG = "app_mode";
static app_mode_t s_current_mode = APP_MODE_OFFLINE;
static EventGroupHandle_t s_mode_event_group = NULL;

static void app_mode_manager_notify(app_mode_t mode)
{
    if (s_mode_event_group == NULL) {
        return;
    }

    EventBits_t bits = (mode == APP_MODE_ONLINE) ? APP_MODE_EVENT_ONLINE_REQUESTED : APP_MODE_EVENT_OFFLINE_REQUESTED;
    xEventGroupSetBits(s_mode_event_group, bits);
}

const char *app_mode_manager_to_string(app_mode_t mode)
{
    switch (mode) {
    case APP_MODE_ONLINE:
        return "online";
    case APP_MODE_OFFLINE:
    default:
        return "offline";
    }
}

esp_err_t app_mode_manager_parse(const char *value, app_mode_t *mode)
{
    if (value == NULL || mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(value, "online") == 0) {
        *mode = APP_MODE_ONLINE;
        return ESP_OK;
    }

    if (strcmp(value, "offline") == 0) {
        *mode = APP_MODE_OFFLINE;
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

esp_err_t app_mode_manager_init(void)
{
    if (s_mode_event_group == NULL) {
        s_mode_event_group = xEventGroupCreate();
        if (s_mode_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_MODE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo abrir NVS: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t stored_mode = APP_MODE_OFFLINE;
    err = nvs_get_u8(handle, APP_MODE_KEY, &stored_mode);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_current_mode = APP_MODE_OFFLINE;
        err = nvs_set_u8(handle, APP_MODE_KEY, (uint8_t)s_current_mode);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        ESP_LOGI(TAG, "Modo no configurado, usando %s", app_mode_manager_to_string(s_current_mode));
    } else if (err == ESP_OK) {
        if (stored_mode == APP_MODE_ONLINE) {
            s_current_mode = APP_MODE_ONLINE;
        } else {
            s_current_mode = APP_MODE_OFFLINE;
        }
        ESP_LOGI(TAG, "Modo cargado: %s", app_mode_manager_to_string(s_current_mode));
    }

    nvs_close(handle);
    if (err == ESP_OK) {
        app_mode_manager_notify(s_current_mode);
    }
    return err;
}

esp_err_t app_mode_manager_get(app_mode_t *mode)
{
    if (mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *mode = s_current_mode;
    return ESP_OK;
}

esp_err_t app_mode_manager_set(app_mode_t mode)
{
    if (mode != APP_MODE_OFFLINE && mode != APP_MODE_ONLINE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (mode == s_current_mode) {
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_MODE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo abrir NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, APP_MODE_KEY, (uint8_t)mode);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        s_current_mode = mode;
        ESP_LOGI(TAG, "Modo actualizado: %s", app_mode_manager_to_string(s_current_mode));
        app_mode_manager_notify(s_current_mode);
    }

    return err;
}

void app_mode_manager_apply_committed(app_mode_t mode)
{
    if (mode != APP_MODE_OFFLINE && mode != APP_MODE_ONLINE) return;
    s_current_mode = mode;
    ESP_LOGI(TAG, "Modo transaccional aplicado: %s", app_mode_manager_to_string(mode));
    app_mode_manager_notify(mode);
}

EventGroupHandle_t app_mode_manager_event_group(void)
{
    return s_mode_event_group;
}
