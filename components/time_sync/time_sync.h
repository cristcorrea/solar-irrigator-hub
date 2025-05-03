#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>
#include <stdbool.h>

void time_sync_init(const char *timezone);
bool time_sync_is_synchronized(void);
struct tm time_sync_get_time(void);

#ifdef __cplusplus
}
#endif
