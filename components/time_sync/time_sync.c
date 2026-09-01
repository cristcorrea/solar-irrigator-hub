#include "time_sync.h"
#include <esp_log.h>
#include <esp_sntp.h>
#include <esp_event.h>
#include <esp_system.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "nvs.h"
#include "freertos/task.h"

#define TAG "TIME_SYNC"

extern SemaphoreHandle_t semaforo_time_listo;

static bool time_synced = false;
static bool time_valid = false;
static bool time_sync_started = false;
static bool persist_task_started = false;
static char saved_timezone[64] = "CET-1CEST,M3.5.0/2,M10.5.0/3";

static esp_err_t persist_time(void)
{
    time_t now = time(NULL);
    if (!time_valid || now < 1577836800) return ESP_OK;
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open("clock", NVS_READWRITE, &h);
    if (err == ESP_OK) err = nvs_set_i64(h, "epoch", (int64_t)now);
    if (err == ESP_OK) err = nvs_set_str(h, "tz", saved_timezone);
    if (err == ESP_OK) err = nvs_set_u8(h, "valid", 1);
    if (err == ESP_OK) err = nvs_commit(h);
    if (h) nvs_close(h);
    return err;
}

static void persist_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10 * 60 * 1000));
        ESP_ERROR_CHECK_WITHOUT_ABORT(persist_time());
    }
}

static void ensure_persist_task(void)
{
    if (!persist_task_started &&
        xTaskCreate(persist_task, "time_persist", 3072, NULL, 4, NULL) == pdPASS) {
        persist_task_started = true;
    }
}

static void time_sync_notification_cb(struct timeval *tv)
{
    time_synced = true;
    time_valid = true;
    ESP_ERROR_CHECK_WITHOUT_ABORT(persist_time());
    ESP_LOGI(TAG, "Hora sincronizada con SNTP");
    if (semaforo_time_listo != NULL) xSemaphoreGive(semaforo_time_listo);
}

esp_err_t time_sync_restore(void)
{
    ensure_persist_task();
    nvs_handle_t h = 0;
    uint8_t valid = 0;
    int64_t epoch = 0;
    size_t tz_len = sizeof(saved_timezone);
    esp_err_t err = nvs_open("clock", NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err == ESP_OK) err = nvs_get_u8(h, "valid", &valid);
    if (err == ESP_OK) err = nvs_get_i64(h, "epoch", &epoch);
    if (err == ESP_OK) err = nvs_get_str(h, "tz", saved_timezone, &tz_len);
    if (h) nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (valid != 1 || epoch < 1577836800) return ESP_OK;
    struct timeval tv = {.tv_sec = (time_t)epoch};
    if (settimeofday(&tv, NULL) != 0) return ESP_FAIL;
    setenv("TZ", saved_timezone, 1);
    tzset();
    time_valid = true;
    ESP_LOGI(TAG, "Reloj restaurado desde NVS");
    return ESP_OK;
}

esp_err_t time_sync_set(time_t unix_time, const char *timezone)
{
    if (unix_time < 1577836800 || (timezone && strlen(timezone) >= sizeof(saved_timezone))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timezone && timezone[0]) strcpy(saved_timezone, timezone);
    setenv("TZ", saved_timezone, 1);
    tzset();
    struct timeval tv = {.tv_sec = unix_time};
    if (settimeofday(&tv, NULL) != 0) return ESP_FAIL;
    time_valid = true;
    ensure_persist_task();
    return persist_time();
}

void time_sync_init(const char *timezone)
{
    if (time_sync_started) {
        return;
    }

    ESP_LOGI(TAG, "Inicializando sincronización de hora...");

    if (timezone != NULL && strlen(timezone) < sizeof(saved_timezone)) {
        strcpy(saved_timezone, timezone);
    }
    setenv("TZ", saved_timezone, 1);
    tzset();
    ensure_persist_task();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
    time_sync_started = true;
}   

bool time_sync_is_synchronized(void)
{
    return time_synced;
}

bool time_sync_is_valid(void)
{
    return time_valid;
}

struct tm time_sync_get_time(void)
{
    time_t now;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    return timeinfo;
}
