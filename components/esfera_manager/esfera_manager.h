#pragma once

#include <inttypes.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t esfera_manager_init(void);
esp_err_t esfera_manager_add(const char *raw_payload, const char *mac_origen);
char *esfera_manager_page_json(size_t max, uint32_t *page_id,
                               uint32_t *last_seq, size_t *pend);
esp_err_t esfera_manager_ack(uint32_t page_id, uint32_t last_seq);
esp_err_t esfera_manager_erase_all(void);
size_t esfera_manager_count(void);
#if CONFIG_TELEMETRY_LOG_TEST_MODE
esp_err_t esfera_manager_debug_fill(size_t count, size_t *written);
#endif
esp_err_t esfera_manager_store_config(const char *payload, size_t payload_len, char normalized_mac[13]);
esp_err_t esfera_manager_register_mac(const char *mac);
bool esfera_manager_is_registered(const char *mac);
