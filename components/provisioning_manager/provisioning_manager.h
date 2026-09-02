#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "app_mode_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROVISIONING_TOKEN_BYTES 16
#define PROVISIONING_TOKEN_HEX_LEN 32

typedef enum {
    PROVISIONING_VIRGIN = 0,
    PROVISIONING_PENDING = 1,
    PROVISIONING_PROVISIONED = 2,
} provisioning_state_t;

esp_err_t provisioning_manager_init(void);
provisioning_state_t provisioning_manager_state(void);
esp_err_t provisioning_manager_begin(app_mode_t mode,
                                     char token_hex[PROVISIONING_TOKEN_HEX_LEN + 1]);
esp_err_t provisioning_manager_confirm(const char *token_hex);
bool provisioning_manager_token_matches(const char *token_hex);
const char *provisioning_manager_state_name(provisioning_state_t state);

#ifdef __cplusplus
}
#endif
