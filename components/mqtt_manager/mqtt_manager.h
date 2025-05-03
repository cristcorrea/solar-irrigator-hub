#include <stdint.h>
#include <stdbool.h>
#include "mqtt_client.h"
#include "esp_now.h"

/**
 * @brief Inicializa la conexión MQTT con HiveMQ.
 */
void mqtt_manager_init(void);

/**
 * @brief Publica un dato de sensor en formato Influx Line Protocol.
 * 
 * @param mac Dirección MAC del dispositivo.
 * @param temperatura Temperatura medida.
 * @param humedad Humedad medida.
 * @param voltaje Voltaje de batería.
 * @param riego Estado del riego (0 o 1).
 */
void mqtt_manager_publicar_datos(const char* mac, float temperatura, float humedad, float voltaje, int riego);


/**
 * @brief Se suscribe a un topic.
 * 
 * @param topic Topic a suscribirse.
 */
void mqtt_manager_suscribirse(char * topic); 

esp_mqtt_client_handle_t mqtt_manager_obtener_cliente(void);

void mqtt_manager_handle_json_config(const char *payload);

void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len); 