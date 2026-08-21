/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/****************************************************************************
 * This is a demo for bluetooth config wifi connection to ap. You can config ESP32 to connect a softap
 * or config ESP32 as a softap to be connected by other device. APP can be downloaded from github
 * android source code: https://github.com/EspressifApp/EspBlufi
 * iOS source code: https://github.com/EspressifApp/EspBlufiForiOS
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt.h"
#endif

#include "esp_blufi_api.h"
#include "blufi_manager.h"
#include "esp_blufi.h"
#include "cJSON.h"
#include "app_mode_manager.h"
#include "esfera_manager.h"

#define EXAMPLE_INVALID_REASON 255
#define EXAMPLE_INVALID_RSSI -128

static void example_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);

#define WIFI_LIST_NUM 10

static wifi_config_t sta_config;
static wifi_config_t ap_config;

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;


/* store the station info for send back to phone */
static bool gl_sta_connected = false;
static bool gl_sta_got_ip = false;
static bool ble_is_connected = false;
static uint8_t gl_sta_bssid[6];
static uint8_t gl_sta_ssid[32];
static int gl_sta_ssid_len;
static wifi_sta_list_t gl_sta_list;
static bool gl_sta_is_connecting = false;
static esp_blufi_extra_info_t gl_sta_conn_info;

extern SemaphoreHandle_t semaforo_wifi_listo;
extern char mac_local[13];

static void blufi_send_json_response(const char *cmd, const char *status, const char *field, const char *value, const char *error)
{
    char response[160];
    int len;

    if (error != NULL) {
        len = snprintf(response, sizeof(response),
                       "{\"cmd\":\"%s\",\"status\":\"%s\",\"error\":\"%s\"}",
                       cmd, status, error);
    } else if (field != NULL && value != NULL) {
        len = snprintf(response, sizeof(response),
                       "{\"cmd\":\"%s\",\"status\":\"%s\",\"%s\":\"%s\"}",
                       cmd, status, field, value);
    } else {
        len = snprintf(response, sizeof(response),
                       "{\"cmd\":\"%s\",\"status\":\"%s\"}",
                       cmd, status);
    }

    if (len <= 0 || len >= (int)sizeof(response)) {
        BLUFI_ERROR("No se pudo generar respuesta BLUFI\n");
        return;
    }

    esp_blufi_send_custom_data((uint8_t *)response, strlen(response));
}

static void blufi_send_mode_response(const char *cmd, const char *status, app_mode_t mode, const char *error)
{
    blufi_send_json_response(cmd, status, "mode", app_mode_manager_to_string(mode), error);
}

static void blufi_set_system_time(time_t unix_time, const char *timezone)
{
    struct timeval tv = {
        .tv_sec = unix_time,
        .tv_usec = 0,
    };

    settimeofday(&tv, NULL);

    if (timezone != NULL && timezone[0] != '\0') {
        setenv("TZ", timezone, 1);
        tzset();
    }
}

static void blufi_handle_custom_json(const uint8_t *data, uint32_t data_len)
{
    if (data == NULL || data_len == 0 || data_len > 512) {
        blufi_send_mode_response("unknown", "error", APP_MODE_OFFLINE, "invalid_length");
        return;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)data, data_len);
    if (root == NULL) {
        blufi_send_mode_response("unknown", "error", APP_MODE_OFFLINE, "invalid_json");
        return;
    }

    cJSON *data_item = cJSON_GetObjectItemCaseSensitive(root, "Data");
    if (data_item != NULL && !cJSON_IsBool(data_item)) {
        cJSON_Delete(root);
        blufi_send_json_response("get_data", "error", NULL, NULL, "invalid_data_request");
        return;
    }
    if (cJSON_IsTrue(data_item)) {
        size_t entry_count = 0;
        char *json = esfera_manager_generate_json(&entry_count);
        cJSON_Delete(root);
        if (json == NULL) {
            blufi_send_json_response("get_data", "error", NULL, NULL, "no_memory");
            return;
        }

        esp_err_t send_err = esp_blufi_send_custom_data((uint8_t *)json, strlen(json));
        free(json);
        if (send_err != ESP_OK) {
            BLUFI_ERROR("No se pudo enviar telemetría por BLUFI: %s\n", esp_err_to_name(send_err));
            return;
        }

        if (entry_count > 0) {
            esp_err_t remove_err = esfera_manager_remove_oldest(entry_count);
            if (remove_err != ESP_OK) {
                BLUFI_ERROR("Telemetría enviada, pero no retirada de NVS: %s\n", esp_err_to_name(remove_err));
            }
        }
        return;
    }

    cJSON *mac_item = cJSON_GetObjectItemCaseSensitive(root, "MACSLAVE");
    if (mac_item != NULL) {
        char mac[13];
        esp_err_t err = esfera_manager_store_config((const char *)data, data_len, mac);
        cJSON_Delete(root);
        if (err != ESP_OK) {
            blufi_send_json_response("set_config", "error", NULL, NULL, esp_err_to_name(err));
            return;
        }
        blufi_send_json_response("set_config", "ok", "MACSLAVE", mac, NULL);
        return;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd_item)) {
        cJSON_Delete(root);
        blufi_send_mode_response("unknown", "error", APP_MODE_OFFLINE, "missing_cmd");
        return;
    }

    if (strlen(cmd_item->valuestring) >= 24) {
        cJSON_Delete(root);
        blufi_send_mode_response("unknown", "error", APP_MODE_OFFLINE, "invalid_cmd");
        return;
    }
    char cmd[24];
    strcpy(cmd, cmd_item->valuestring);
    if (strcmp(cmd, "get_mode") == 0) {
        app_mode_t mode = APP_MODE_OFFLINE;
        esp_err_t err = app_mode_manager_get(&mode);
        cJSON_Delete(root);
        if (err != ESP_OK) {
            blufi_send_mode_response(cmd, "error", mode, esp_err_to_name(err));
            return;
        }
        blufi_send_mode_response(cmd, "ok", mode, NULL);
        return;
    }

    if (strcmp(cmd, "set_mode") == 0) {
        cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
        app_mode_t mode = APP_MODE_OFFLINE;

        if (!cJSON_IsString(mode_item) || app_mode_manager_parse(mode_item->valuestring, &mode) != ESP_OK) {
            cJSON_Delete(root);
            blufi_send_mode_response(cmd, "error", mode, "invalid_mode");
            return;
        }

        esp_err_t err = app_mode_manager_set(mode);
        cJSON_Delete(root);
        if (err != ESP_OK) {
            blufi_send_mode_response(cmd, "error", mode, esp_err_to_name(err));
            return;
        }

        if (mode == APP_MODE_ONLINE) {
            esp_err_t wifi_err = esp_wifi_connect();
            if (wifi_err != ESP_OK) {
                BLUFI_ERROR("No se pudo solicitar conexión Wi-Fi: %s\n", esp_err_to_name(wifi_err));
            }
        } else {
            esp_wifi_disconnect();
        }

        blufi_send_mode_response(cmd, "ok", mode, NULL);
        return;
    }

    if (strcmp(cmd, "set_time") == 0) {
        cJSON *unix_item = cJSON_GetObjectItem(root, "unix");
        cJSON *tz_item = cJSON_GetObjectItem(root, "tz");

        if (!cJSON_IsNumber(unix_item) || !isfinite(unix_item->valuedouble) ||
            unix_item->valuedouble < 1577836800.0 || unix_item->valuedouble > 4102444800.0) {
            cJSON_Delete(root);
            blufi_send_json_response(cmd, "error", NULL, NULL, "invalid_unix");
            return;
        }

        const char *timezone = NULL;
        if (cJSON_IsString(tz_item) && strlen(tz_item->valuestring) <= 63) {
            timezone = tz_item->valuestring;
        } else if (tz_item != NULL) {
            cJSON_Delete(root);
            blufi_send_json_response(cmd, "error", NULL, NULL, "invalid_timezone");
            return;
        }

        blufi_set_system_time((time_t)unix_item->valuedouble, timezone);
        cJSON_Delete(root);
        blufi_send_json_response(cmd, "ok", NULL, NULL, NULL);
        BLUFI_INFO("Hora configurada desde BLE\n");
        return;
    }

    cJSON_Delete(root);
    blufi_send_mode_response(cmd, "error", APP_MODE_OFFLINE, "unknown_cmd");
}

static void example_record_wifi_conn_info(int rssi, uint8_t reason)
{
    memset(&gl_sta_conn_info, 0, sizeof(esp_blufi_extra_info_t));
    if (!gl_sta_is_connecting)
    {
        gl_sta_conn_info.sta_conn_rssi_set = true;
        gl_sta_conn_info.sta_conn_rssi = rssi;
        gl_sta_conn_info.sta_conn_end_reason_set = true;
        gl_sta_conn_info.sta_conn_end_reason = reason;
    }
}

static void example_wifi_connect(void)
{
    gl_sta_is_connecting = (esp_wifi_connect() == ESP_OK);
    example_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
}

static int softap_get_current_connection_number(void)
{
    esp_err_t ret;
    ret = esp_wifi_ap_get_sta_list(&gl_sta_list);
    if (ret == ESP_OK)
    {
        return gl_sta_list.num;
    }

    return 0;
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    wifi_mode_t mode;

    switch (event_id)
    {
    case IP_EVENT_STA_GOT_IP:
    {
        esp_blufi_extra_info_t info;

        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        esp_wifi_get_mode(&mode);

        memset(&info, 0, sizeof(esp_blufi_extra_info_t));
        memcpy(info.sta_bssid, gl_sta_bssid, 6);
        info.sta_bssid_set = true;
        info.sta_ssid = gl_sta_ssid;
        info.sta_ssid_len = gl_sta_ssid_len;
        gl_sta_got_ip = true;

        ESP_ERROR_CHECK(esp_blufi_send_custom_data((uint8_t *)mac_local, strlen(mac_local)));

        xSemaphoreGive(semaforo_wifi_listo);
        if (ble_is_connected == true)
        {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS, softap_get_current_connection_number(), &info);
        }
        else
        {
            BLUFI_INFO("BLUFI BLE is not connected yet\n");
        }
        break;
    }
    default:
        break;
    }
    return;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    wifi_event_sta_connected_t *event;
    wifi_event_sta_disconnected_t *disconnected_event;
    wifi_mode_t mode;

    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
    {
        app_mode_t current_mode = APP_MODE_OFFLINE;
        if (app_mode_manager_get(&current_mode) == ESP_OK && current_mode == APP_MODE_ONLINE) {
            example_wifi_connect();
        }
        break;
    }
    case WIFI_EVENT_STA_CONNECTED:
        gl_sta_connected = true;
        gl_sta_is_connecting = false;
        event = (wifi_event_sta_connected_t *)event_data;
        memcpy(gl_sta_bssid, event->bssid, 6);
        memcpy(gl_sta_ssid, event->ssid, event->ssid_len);
        gl_sta_ssid_len = event->ssid_len;
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
    {
        disconnected_event = (wifi_event_sta_disconnected_t *)event_data;
        example_record_wifi_conn_info(disconnected_event->rssi, disconnected_event->reason);

        app_mode_t current_mode = APP_MODE_OFFLINE;
        if (app_mode_manager_get(&current_mode) == ESP_OK && current_mode == APP_MODE_ONLINE) {
            gl_sta_is_connecting = (esp_wifi_connect() == ESP_OK);
            if (gl_sta_is_connecting) {
                BLUFI_INFO("Wi-Fi desconectado; reconexión solicitada\n");
            }
        } else {
            gl_sta_is_connecting = false;
        }

        esp_wifi_get_mode(&mode);
        if (ble_is_connected) {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL,
                                            softap_get_current_connection_number(), &gl_sta_conn_info);
        }

        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        gl_sta_connected = false;
        gl_sta_got_ip = false;
        memset(gl_sta_ssid, 0, 32);
        memset(gl_sta_bssid, 0, 6);
        gl_sta_ssid_len = 0;
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    }
    case WIFI_EVENT_AP_START:
        esp_wifi_get_mode(&mode);

        /* TODO: get config or information of softap, then set to report extra_info */
        if (ble_is_connected == true)
        {
            if (gl_sta_connected)
            {
                esp_blufi_extra_info_t info;
                memset(&info, 0, sizeof(esp_blufi_extra_info_t));
                memcpy(info.sta_bssid, gl_sta_bssid, 6);
                info.sta_bssid_set = true;
                info.sta_ssid = gl_sta_ssid;
                info.sta_ssid_len = gl_sta_ssid_len;
                esp_blufi_send_wifi_conn_report(mode, gl_sta_got_ip ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP, softap_get_current_connection_number(), &info);
            }
            else if (gl_sta_is_connecting)
            {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, softap_get_current_connection_number(), &gl_sta_conn_info);
            }
            else
            {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, softap_get_current_connection_number(), &gl_sta_conn_info);
            }
        }
        else
        {
            BLUFI_INFO("BLUFI BLE is not connected yet\n");
        }
        break;
    case WIFI_EVENT_SCAN_DONE:
    {
        uint16_t apCount = 0;
        esp_wifi_scan_get_ap_num(&apCount);
        if (apCount == 0)
        {
            BLUFI_INFO("Nothing AP found");
            break;
        }
        wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * apCount);
        if (!ap_list)
        {
            BLUFI_ERROR("malloc error, ap_list is NULL");
            esp_wifi_clear_ap_list();
            break;
        }
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&apCount, ap_list));
        esp_blufi_ap_record_t *blufi_ap_list = (esp_blufi_ap_record_t *)malloc(apCount * sizeof(esp_blufi_ap_record_t));
        if (!blufi_ap_list)
        {
            if (ap_list)
            {
                free(ap_list);
            }
            BLUFI_ERROR("malloc error, blufi_ap_list is NULL");
            break;
        }
        for (int i = 0; i < apCount; ++i)
        {
            blufi_ap_list[i].rssi = ap_list[i].rssi;
            memcpy(blufi_ap_list[i].ssid, ap_list[i].ssid, sizeof(ap_list[i].ssid));
        }

        if (ble_is_connected == true)
        {
            esp_blufi_send_wifi_list(apCount, blufi_ap_list);
        }
        else
        {
            BLUFI_INFO("BLUFI BLE is not connected yet\n");
        }

        esp_wifi_scan_stop();
        free(ap_list);
        free(blufi_ap_list);
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED:
    {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        BLUFI_INFO("station " MACSTR " join, AID=%d", MAC2STR(event->mac), event->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED:
    {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        BLUFI_INFO("station " MACSTR " leave, AID=%d, reason=%d", MAC2STR(event->mac), event->aid, event->reason);
        break;
    }

    default:
        break;
    }
    return;
}

static void initialise_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    example_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
    ESP_ERROR_CHECK(esp_wifi_start());

    app_mode_t current_mode = APP_MODE_OFFLINE;
    if (app_mode_manager_get(&current_mode) == ESP_OK && current_mode == APP_MODE_OFFLINE) {
        esp_err_t channel_err = esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
        if (channel_err != ESP_OK) {
            BLUFI_ERROR("No se pudo fijar el canal ESP-NOW offline: %s\n", esp_err_to_name(channel_err));
        }
    }
}

static esp_blufi_callbacks_t example_callbacks = {
    .event_cb = example_event_callback,
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,
    .encrypt_func = blufi_aes_encrypt,
    .decrypt_func = blufi_aes_decrypt,
    .checksum_func = blufi_crc_checksum,
};

static void example_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    /* actually, should post to blufi_task handle the procedure,
     * now, as a example, we do it more simply */
    switch (event)
    {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        BLUFI_INFO("BLUFI init finish\n");

        esp_blufi_adv_start();
        break;
    case ESP_BLUFI_EVENT_DEINIT_FINISH:
        BLUFI_INFO("BLUFI deinit finish\n");
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        BLUFI_INFO("BLUFI ble connect\n");
        ble_is_connected = true;
        esp_blufi_adv_stop();
        blufi_security_init();
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        BLUFI_INFO("BLUFI ble disconnect\n");
        ble_is_connected = false;
        blufi_security_deinit();
        esp_blufi_adv_start();
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        BLUFI_INFO("BLUFI Set WIFI opmode %d\n", param->wifi_mode.op_mode);
        ESP_ERROR_CHECK(esp_wifi_set_mode(param->wifi_mode.op_mode));
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        BLUFI_INFO("BLUFI requset wifi connect to AP\n");
        /* there is no wifi callback when the device has already connected to this wifi
        so disconnect wifi before connection.
        */
        esp_wifi_disconnect();
        example_wifi_connect();
        break;
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        BLUFI_INFO("BLUFI requset wifi disconnect from AP\n");
        esp_wifi_disconnect();
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        BLUFI_ERROR("BLUFI report error, error code %d\n", param->report_error.state);
        esp_blufi_send_error_info(param->report_error.state);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
    {
        wifi_mode_t mode;
        esp_blufi_extra_info_t info;

        esp_wifi_get_mode(&mode);

        if (gl_sta_connected)
        {
            memset(&info, 0, sizeof(esp_blufi_extra_info_t));
            memcpy(info.sta_bssid, gl_sta_bssid, 6);
            info.sta_bssid_set = true;
            info.sta_ssid = gl_sta_ssid;
            info.sta_ssid_len = gl_sta_ssid_len;
            esp_blufi_send_wifi_conn_report(mode, gl_sta_got_ip ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP, softap_get_current_connection_number(), &info);
        }
        else if (gl_sta_is_connecting)
        {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, softap_get_current_connection_number(), &gl_sta_conn_info);
        }
        else
        {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, softap_get_current_connection_number(), &gl_sta_conn_info);
        }
        BLUFI_INFO("BLUFI get wifi status from AP\n");

        break;
    }
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        BLUFI_INFO("blufi close a gatt connection");
        esp_blufi_disconnect();
        break;
    case ESP_BLUFI_EVENT_DEAUTHENTICATE_STA:
        /* TODO */
        break;
    case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        memcpy(sta_config.sta.bssid, param->sta_bssid.bssid, 6);
        sta_config.sta.bssid_set = 1;
        sta_config.sta.scan_method = WIFI_FAST_SCAN; // usa solo el canal indicado
        sta_config.sta.channel = 6;                  // fuerza canal 6
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        BLUFI_INFO("Recv STA BSSID %s\n", sta_config.sta.ssid);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_SSID:
    {
        size_t ssid_len = param->sta_ssid.ssid_len;
        if (ssid_len == 0 || ssid_len > sizeof(sta_config.sta.ssid)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(sta_config.sta.ssid, 0, sizeof(sta_config.sta.ssid));
        memcpy(sta_config.sta.ssid, param->sta_ssid.ssid, ssid_len);
        sta_config.sta.scan_method = WIFI_FAST_SCAN; // usa solo el canal indicado
        sta_config.sta.channel = 6;                  // fuerza canal 6
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        BLUFI_INFO("Recv STA SSID %.*s\n", (int)ssid_len, sta_config.sta.ssid);
        break;
    }
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
    {
        size_t password_len = param->sta_passwd.passwd_len;
        if (password_len > sizeof(sta_config.sta.password)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(sta_config.sta.password, 0, sizeof(sta_config.sta.password));
        memcpy(sta_config.sta.password, param->sta_passwd.passwd, password_len);
        sta_config.sta.scan_method = WIFI_FAST_SCAN; // usa solo el canal indicado
        sta_config.sta.channel = 6;                  // fuerza canal 6
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        BLUFI_INFO("Recv STA PASSWORD len=%u\n", (unsigned)password_len);
        break;
    }
    case ESP_BLUFI_EVENT_RECV_SOFTAP_SSID:
        strncpy((char *)ap_config.ap.ssid, (char *)param->softap_ssid.ssid, param->softap_ssid.ssid_len);
        ap_config.ap.ssid[param->softap_ssid.ssid_len] = '\0';
        ap_config.ap.ssid_len = param->softap_ssid.ssid_len;
        sta_config.sta.scan_method = WIFI_FAST_SCAN; // usa solo el canal indicado
        sta_config.sta.channel = 6;                  // fuerza canal 6
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP SSID %s, ssid len %d\n", ap_config.ap.ssid, ap_config.ap.ssid_len);
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_PASSWD:
        strncpy((char *)ap_config.ap.password, (char *)param->softap_passwd.passwd, param->softap_passwd.passwd_len);
        ap_config.ap.password[param->softap_passwd.passwd_len] = '\0';
        sta_config.sta.scan_method = WIFI_FAST_SCAN; // usa solo el canal indicado
        sta_config.sta.channel = 6;                  // fuerza canal 6
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP PASSWORD len=%d\n", param->softap_passwd.passwd_len);
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_MAX_CONN_NUM:
        if (param->softap_max_conn_num.max_conn_num > 4)
        {
            return;
        }
        ap_config.ap.max_connection = param->softap_max_conn_num.max_conn_num;
        sta_config.sta.scan_method = WIFI_FAST_SCAN; // usa solo el canal indicado
        sta_config.sta.channel = 6;                  // fuerza canal 6
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP MAX CONN NUM %d\n", ap_config.ap.max_connection);
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_AUTH_MODE:
        if (param->softap_auth_mode.auth_mode >= WIFI_AUTH_MAX)
        {
            return;
        }
        ap_config.ap.authmode = param->softap_auth_mode.auth_mode;
        sta_config.sta.scan_method = WIFI_FAST_SCAN; // usa solo el canal indicado
        sta_config.sta.channel = 6;                  // fuerza canal 6
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP AUTH MODE %d\n", ap_config.ap.authmode);
        break;
    case ESP_BLUFI_EVENT_RECV_SOFTAP_CHANNEL:
        if (param->softap_channel.channel > 13)
        {
            return;
        }
        ap_config.ap.channel = param->softap_channel.channel;
        sta_config.sta.scan_method = WIFI_FAST_SCAN; // usa solo el canal indicado
        sta_config.sta.channel = 6;                  // fuerza canal 6
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP CHANNEL %d\n", ap_config.ap.channel);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_LIST:
    {
        wifi_scan_config_t scanConf = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false};
        esp_err_t ret = esp_wifi_scan_start(&scanConf, true);
        if (ret != ESP_OK)
        {
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
        break;
    }
    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
        BLUFI_INFO("Recv Custom Data %" PRIu32 "\n", param->custom_data.data_len);
        blufi_handle_custom_json(param->custom_data.data, param->custom_data.data_len);
        break;
    case ESP_BLUFI_EVENT_RECV_USERNAME:
        /* Not handle currently */
        break;
    case ESP_BLUFI_EVENT_RECV_CA_CERT:
        /* Not handle currently */
        break;
    case ESP_BLUFI_EVENT_RECV_CLIENT_CERT:
        /* Not handle currently */
        break;
    case ESP_BLUFI_EVENT_RECV_SERVER_CERT:
        /* Not handle currently */
        break;
    case ESP_BLUFI_EVENT_RECV_CLIENT_PRIV_KEY:
        /* Not handle currently */
        break;
        ;
    case ESP_BLUFI_EVENT_RECV_SERVER_PRIV_KEY:
        /* Not handle currently */
        break;
    default:
        break;
    }
}

esp_err_t blufi_init(void)
{
    esp_err_t ret;

    initialise_wifi();

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    ret = esp_blufi_controller_init();
    if (ret)
    {
        BLUFI_ERROR("%s BLUFI controller init failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }
#endif

    ret = esp_blufi_host_and_cb_init(&example_callbacks);
    if (ret)
    {
        BLUFI_ERROR("%s initialise failed: %s\n", __func__, esp_err_to_name(ret));
        return ret;
    }

    BLUFI_INFO("BLUFI VERSION %04x\n", esp_blufi_get_version());
    return ESP_OK;
}
