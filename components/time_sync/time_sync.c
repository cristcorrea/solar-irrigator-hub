#include "time_sync.h"
#include <esp_log.h>
#include <esp_sntp.h>
#include <esp_event.h>
#include <esp_system.h>

#define TAG "TIME_SYNC"

extern SemaphoreHandle_t semaforo_time_listo;

static bool time_synced = false;

static void time_sync_notification_cb(struct timeval *tv)
{
    time_synced = true;
    ESP_LOGI(TAG, "Hora sincronizada con SNTP");
    xSemaphoreGive(semaforo_time_listo);
}

void time_sync_init(const char *timezone)
{
    ESP_LOGI(TAG, "Inicializando sincronización de hora...");

    setenv("TZ", timezone, 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
}   

bool time_sync_is_synchronized(void)
{
    return time_synced;
}

struct tm time_sync_get_time(void)
{
    time_t now;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    return timeinfo;
}
