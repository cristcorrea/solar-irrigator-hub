#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>
#include <stdbool.h>
#include "esp_err.h"

void time_sync_init(const char *timezone);
esp_err_t time_sync_restore(void);
esp_err_t time_sync_set(time_t unix_time, const char *timezone);
bool time_sync_is_synchronized(void);
bool time_sync_is_valid(void);
struct tm time_sync_get_time(void);

#ifdef __cplusplus
}
#endif
