#include "provisioning_manager.h"

#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define CONFIG_NAMESPACE "hub_config"
#define MODE_KEY "work_mode"
#define STATE_KEY "prov_state"
#define TOKEN_KEY "prov_token"
#define ALTA_TIMEOUT_US (120LL * 1000LL * 1000LL)

static const char *TAG = "PROVISIONING";
static provisioning_state_t s_state = PROVISIONING_VIRGIN;
static uint8_t s_token[PROVISIONING_TOKEN_BYTES];
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_timeout;

static void token_to_hex(const uint8_t *token, char *hex)
{
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < PROVISIONING_TOKEN_BYTES; ++i) {
        hex[i * 2] = digits[token[i] >> 4];
        hex[i * 2 + 1] = digits[token[i] & 0x0f];
    }
    hex[PROVISIONING_TOKEN_HEX_LEN] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool token_from_hex(const char *hex, uint8_t token[PROVISIONING_TOKEN_BYTES])
{
    if (hex == NULL || strlen(hex) != PROVISIONING_TOKEN_HEX_LEN) return false;
    for (size_t i = 0; i < PROVISIONING_TOKEN_BYTES; ++i) {
        int hi = hex_value(hex[i * 2]);
        int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        token[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool token_matches_locked(const char *token_hex)
{
    uint8_t candidate[PROVISIONING_TOKEN_BYTES] = {0};
    bool valid = token_from_hex(token_hex, candidate);
    uint8_t difference = valid ? 0 : 1;
    for (size_t i = 0; i < sizeof(candidate); ++i) difference |= candidate[i] ^ s_token[i];
    return valid && difference == 0;
}

static esp_err_t persist_virgin_locked(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, TOKEN_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_set_u8(handle, STATE_KEY, PROVISIONING_VIRGIN);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        memset(s_token, 0, sizeof(s_token));
        s_state = PROVISIONING_VIRGIN;
    }
    return err;
}

static void alta_timeout_cb(void *arg)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    if (s_state == PROVISIONING_PENDING) {
        esp_err_t err = persist_virgin_locked();
        if (err == ESP_OK) ESP_LOGW(TAG, "Alta vencida; estado restaurado a VIRGEN");
        else ESP_LOGE(TAG, "No se pudo cancelar alta vencida: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
}

const char *provisioning_manager_state_name(provisioning_state_t state)
{
    switch (state) {
    case PROVISIONING_PENDING: return "PENDING";
    case PROVISIONING_PROVISIONED: return "PROVISIONED";
    default: return "VIRGEN";
    }
}

esp_err_t provisioning_manager_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    const esp_timer_create_args_t timer_args = {
        .callback = alta_timeout_cb,
        .name = "alta_timeout",
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_timeout);
    if (err != ESP_OK) return err;

    nvs_handle_t handle = 0;
    err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    uint8_t stored = PROVISIONING_VIRGIN;
    err = nvs_get_u8(handle, STATE_KEY, &stored);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_set_u8(handle, STATE_KEY, PROVISIONING_VIRGIN);
        if (err == ESP_OK) err = nvs_commit(handle);
        stored = PROVISIONING_VIRGIN;
    }
    bool repair_to_virgin = false;
    if (err == ESP_OK && stored == PROVISIONING_PROVISIONED) {
        size_t token_size = sizeof(s_token);
        err = nvs_get_blob(handle, TOKEN_KEY, s_token, &token_size);
        if (err != ESP_OK || token_size != sizeof(s_token)) {
            stored = PROVISIONING_VIRGIN;
            repair_to_virgin = true;
        }
    }
    nvs_close(handle);
    if (err != ESP_OK && stored != PROVISIONING_VIRGIN) return err;

    s_state = (provisioning_state_t)stored;
    if (s_state == PROVISIONING_PENDING || s_state > PROVISIONING_PROVISIONED || repair_to_virgin) {
        if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
        err = persist_virgin_locked();
        xSemaphoreGive(s_lock);
        if (err != ESP_OK) return err;
        ESP_LOGW(TAG, "Alta pendiente encontrada al arrancar; vuelve a VIRGEN");
    } else if (s_state == PROVISIONING_VIRGIN) {
        memset(s_token, 0, sizeof(s_token));
    }
    ESP_LOGI(TAG, "Estado de vinculación: %s", provisioning_manager_state_name(s_state));
    return ESP_OK;
}

provisioning_state_t provisioning_manager_state(void)
{
    return s_state;
}

esp_err_t provisioning_manager_begin(app_mode_t mode,
                                     char token_hex[PROVISIONING_TOKEN_HEX_LEN + 1])
{
    if (token_hex == NULL || (mode != APP_MODE_ONLINE && mode != APP_MODE_OFFLINE)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    if (s_state == PROVISIONING_PROVISIONED) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t new_token[PROVISIONING_TOKEN_BYTES];
    esp_fill_random(new_token, sizeof(new_token));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_blob(handle, TOKEN_KEY, new_token, sizeof(new_token));
    if (err == ESP_OK) err = nvs_set_u8(handle, MODE_KEY, (uint8_t)mode);
    if (err == ESP_OK) err = nvs_set_u8(handle, STATE_KEY, PROVISIONING_PENDING);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    if (err == ESP_OK) {
        memcpy(s_token, new_token, sizeof(s_token));
        s_state = PROVISIONING_PENDING;
        app_mode_manager_apply_committed(mode);
        token_to_hex(s_token, token_hex);
        esp_timer_stop(s_timeout);
        err = esp_timer_start_once(s_timeout, ALTA_TIMEOUT_US);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo iniciar timeout de alta: %s", esp_err_to_name(err));
            esp_err_t rollback_err = persist_virgin_locked();
            if (rollback_err != ESP_OK) {
                ESP_LOGE(TAG, "También falló rollback a VIRGEN: %s", esp_err_to_name(rollback_err));
            }
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

bool provisioning_manager_token_matches(const char *token_hex)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return false;
    bool matches = s_state != PROVISIONING_VIRGIN && token_matches_locked(token_hex);
    xSemaphoreGive(s_lock);
    return matches;
}

esp_err_t provisioning_manager_confirm(const char *token_hex)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    if (s_state != PROVISIONING_PENDING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!token_matches_locked(token_hex)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_CRC;
    }
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_u8(handle, STATE_KEY, PROVISIONING_PROVISIONED);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    if (err == ESP_OK) {
        s_state = PROVISIONING_PROVISIONED;
        esp_timer_stop(s_timeout);
        ESP_LOGI(TAG, "Alta confirmada; estado PROVISIONED");
    }
    xSemaphoreGive(s_lock);
    return err;
}
