// main/hub_station.h
#ifndef HUB_STATION_H
#define HUB_STATION_H

#include <stdbool.h>
#include "esp_err.h"

esp_err_t hub_iniciar_espnow(void);
void hub_enviar_broadcast_descubrimiento(void);
bool hub_aceptar_respuesta_esfera(void);

#endif // HUB_STATION_H
