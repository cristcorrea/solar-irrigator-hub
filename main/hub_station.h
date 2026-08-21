// main/hub_station.h
#ifndef HUB_STATION_H
#define HUB_STATION_H

#include "esp_err.h"

esp_err_t hub_iniciar_espnow(void);
void hub_enviar_broadcast_descubrimiento(void);

#endif // HUB_STATION_H
