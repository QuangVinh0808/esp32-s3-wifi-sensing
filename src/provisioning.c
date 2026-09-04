#include "provisioning.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config_store.h"
#include "provisioning_web.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "PROVISIONING";

#define PROVISIONING_AP_PASSWORD       "configure123"
#define PROVISIONING_AP_CHANNEL        1
#define PROVISIONING_AP_MAX_CLIENTS    4

#define PROVISIONING_MAX_SCAN_RESULTS  15
#define PROVISIONING_POST_BUFFER_SIZE  384
#define PROVISIONING_RESTART_DELAY_MS  2000

static esp_netif_t *s_ap_netif = NULL;
static httpd_handle_t s_http_server = NULL;

static bool s_running = false;
static bool s_restart_scheduled = false;

static char s_ap_ssid[33] = {0};

static int hex_to_value(char character)
{
    if ((character >= '0') && (character <= '9'))
    {
        return character - '0';
    }

    if ((character >= 'a') && (character <= 'f'))
    {
        return character - 'a' + 10;
    }

    if ((character >= 'A') && (character <= 'F'))
    {
        return character - 'A' + 10;
    }

    return -1;
}

static bool url_decode(
    const char *source,
    char *destination,
    size_t destination_size
)
{
    size_t source_index = 0;
    size_t destination_index = 0;

    if ((source == NULL) ||
        (destination == NULL) ||
        (destination_size == 0))
    {
        return false;
    }

    while (source[source_index] != '\0')
    {
        char decoded_character;

        if (source[source_index] == '+')
        {
            decoded_character = ' ';
            source_index++;
        }
        else if (source[source_index] == '%')
        {
            int high;
            int low;

            if ((source[source_index + 1] == '\0') ||
                (source[source_index + 2] == '\0'))
            {
                return false;
            }

            high = hex_to_value(
                source[source_index + 1]
            );

            low = hex_to_value(
                source[source_index + 2]
            );

            if ((high < 0) || (low < 0))
            {
                return false;
            }

            decoded_character =
                (char)((high << 4) | low);

            source_index += 3;
        }
        else
        {
            decoded_character =
                source[source_index];

            source_index++;
        }

        /*
         * Dành một byte cuối cho '\0'.
         */
        if ((destination_index + 1) >=
            destination_size)
        {
            return false;
        }

        destination[destination_index] =
            decoded_character;

        destination_index++;
    }

    destination[destination_index] = '\0';

    return true;
}

static void json_escape_ssid(
    const uint8_t *ssid,
    char *output,
    size_t output_size
)
{
    size_t input_index = 0;
    size_t output_index = 0;

    if ((ssid == NULL) ||
        (output == NULL) ||
        (output_size == 0))
    {
        return;
    }

    while ((input_index < 32) &&
           (ssid[input_index] != '\0'))
    {
        uint8_t character = ssid[input_index];

        if ((character == '"') ||
            (character == '\\'))
        {
            if ((output_index + 2) >= output_size)
            {
                break;
            }

            output[output_index++] = '\\';
            output[output_index++] = (char)character;
        }
        else if (character >= 0x20)
        {
            if ((output_index + 1) >= output_size)
            {
                break;
            }

            output[output_index++] = (char)character;
        }

        input_index++;
    }

    output[output_index] = '\0';
}

static esp_err_t root_get_handler(
    httpd_req_t *request
)
{
    httpd_resp_set_type(
        request,
        "text/html; charset=utf-8"
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    return httpd_resp_send(
        request,
        PROVISIONING_HTML,
        HTTPD_RESP_USE_STRLEN
    );
}

static esp_err_t scan_get_handler(
    httpd_req_t *request
)
{
    wifi_scan_config_t scan_config = {0};

    wifi_ap_record_t access_points[
        PROVISIONING_MAX_SCAN_RESULTS
    ] = {0};

    uint16_t access_point_count =
        PROVISIONING_MAX_SCAN_RESULTS;

    esp_err_t err;

    scan_config.show_hidden = true;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;

    ESP_LOGI(TAG, "Scanning nearby Wi-Fi networks");

    err = esp_wifi_scan_start(
        &scan_config,
        true
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Wi-Fi scan failed: %s",
            esp_err_to_name(err)
        );

        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Wi-Fi scan failed"
        );
    }

    err = esp_wifi_scan_get_ap_records(
        &access_point_count,
        access_points
    );

    if (err != ESP_OK)
    {
        esp_wifi_clear_ap_list();

        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Cannot read scan results"
        );
    }

    httpd_resp_set_type(
        request,
        "application/json"
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    err = httpd_resp_sendstr_chunk(
        request,
        "["
    );

    if (err != ESP_OK)
    {
        return err;
    }

    for (uint16_t index = 0;
         index < access_point_count;
         index++)
    {
        char escaped_ssid[70] = {0};
        char json_item[160] = {0};

        json_escape_ssid(
            access_points[index].ssid,
            escaped_ssid,
            sizeof(escaped_ssid)
        );

        snprintf(
            json_item,
            sizeof(json_item),
            "%s{\"ssid\":\"%s\","
            "\"rssi\":%d,"
            "\"channel\":%u,"
            "\"auth\":%d}",
            index == 0 ? "" : ",",
            escaped_ssid,
            access_points[index].rssi,
            access_points[index].primary,
            (int)access_points[index].authmode
        );

        err = httpd_resp_sendstr_chunk(
            request,
            json_item
        );

        if (err != ESP_OK)
        {
            return err;
        }
    }

    return httpd_resp_sendstr_chunk(
        request,
        NULL
    );
}

static void delayed_restart_task(
    void *parameter
)
{
    (void)parameter;

    ESP_LOGI(
        TAG,
        "Restarting in %d ms",
        PROVISIONING_RESTART_DELAY_MS
    );

    vTaskDelay(
        pdMS_TO_TICKS(
            PROVISIONING_RESTART_DELAY_MS
        )
    );

    esp_restart();

    vTaskDelete(NULL);
}

static esp_err_t config_post_handler(
    httpd_req_t *request
)
{
    char body[
        PROVISIONING_POST_BUFFER_SIZE
    ] = {0};

    char encoded_ssid[
        (APP_WIFI_SSID_MAX_LEN * 3) + 1
    ] = {0};

    char encoded_password[
        (APP_WIFI_PASSWORD_MAX_LEN * 3) + 1
    ] = {0};

    char decoded_ssid[
        APP_WIFI_SSID_MAX_LEN + 1
    ] = {0};

    char decoded_password[
        APP_WIFI_PASSWORD_MAX_LEN + 1
    ] = {0};

    size_t received_length = 0;

    app_config_t candidate_config;
    esp_err_t err;

    if (s_restart_scheduled)
    {
        return httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "Restart already scheduled"
        );
    }

    if ((request->content_len == 0) ||
        (request->content_len >= sizeof(body)))
    {
        return httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "Invalid request length"
        );
    }

    while (received_length <
           request->content_len)
    {
        int receive_result = httpd_req_recv(
            request,
            body + received_length,
            request->content_len -
                received_length
        );

        if (receive_result ==
            HTTPD_SOCK_ERR_TIMEOUT)
        {
            continue;
        }

        if (receive_result <= 0)
        {
            return ESP_FAIL;
        }

        received_length +=
            (size_t)receive_result;
    }

    body[received_length] = '\0';

    err = httpd_query_key_value(
        body,
        "ssid",
        encoded_ssid,
        sizeof(encoded_ssid)
    );

    if (err != ESP_OK)
    {
        return httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "SSID is missing or too long"
        );
    }

    err = httpd_query_key_value(
        body,
        "password",
        encoded_password,
        sizeof(encoded_password)
    );

    if (err == ESP_ERR_NOT_FOUND)
    {
        encoded_password[0] = '\0';
    }
    else if (err != ESP_OK)
    {
        return httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "Password is too long"
        );
    }

    if (!url_decode(
            encoded_ssid,
            decoded_ssid,
            sizeof(decoded_ssid)))
    {
        return httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "Invalid SSID encoding"
        );
    }

    if (!url_decode(
            encoded_password,
            decoded_password,
            sizeof(decoded_password)))
    {
        return httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "Invalid password encoding"
        );
    }

    /*
     * Giữ lại threshold và packet rate của active config.
     */
    if (config_store_load(
            &candidate_config) != ESP_OK)
    {
        config_store_set_defaults(
            &candidate_config
        );
    }

    memset(
        candidate_config.ssid,
        0,
        sizeof(candidate_config.ssid)
    );

    memset(
        candidate_config.password,
        0,
        sizeof(candidate_config.password)
    );

    memcpy(
        candidate_config.ssid,
        decoded_ssid,
        strlen(decoded_ssid)
    );

    memcpy(
        candidate_config.password,
        decoded_password,
        strlen(decoded_password)
    );

    candidate_config.version =
        APP_CONFIG_VERSION;

    candidate_config.provisioned = true;

    if (!config_store_validate(
            &candidate_config))
    {
        return httpd_resp_send_err(
            request,
            HTTPD_400_BAD_REQUEST,
            "Invalid Wi-Fi configuration"
        );
    }

    err = config_store_save_pending(
        &candidate_config
    );

    if (err != ESP_OK)
    {
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Cannot save configuration"
        );
    }

    /*
     * Không log password.
     */
    ESP_LOGI(
        TAG,
        "New pending SSID received: %s",
        candidate_config.ssid
    );

    BaseType_t task_result = xTaskCreate(
        delayed_restart_task,
        "provision_restart",
        3072,
        NULL,
        5,
        NULL
    );

    if (task_result != pdPASS)
    {
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Saved, but restart task failed"
        );
    }

    s_restart_scheduled = true;

    httpd_resp_set_type(
        request,
        "text/html; charset=utf-8"
    );

    return httpd_resp_send(
        request,
        PROVISIONING_SUCCESS_HTML,
        HTTPD_RESP_USE_STRLEN
    );
}

static const httpd_uri_t ROOT_URI =
{
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t SCAN_URI =
{
    .uri = "/api/scan",
    .method = HTTP_GET,
    .handler = scan_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t CONFIG_URI =
{
    .uri = "/api/config",
    .method = HTTP_POST,
    .handler = config_post_handler,
    .user_ctx = NULL
};

esp_err_t provisioning_start(void)
{
    uint8_t mac_address[6] = {0};

    wifi_config_t ap_config = {0};
    httpd_config_t http_config =
        HTTPD_DEFAULT_CONFIG();

    esp_err_t err;

    if (s_running)
    {
        return ESP_OK;
    }

    err = esp_read_mac(
        mac_address,
        ESP_MAC_WIFI_SOFTAP
    );

    if (err != ESP_OK)
    {
        return err;
    }

    snprintf(
        s_ap_ssid,
        sizeof(s_ap_ssid),
        "ESP32-SENSING-%02X%02X",
        mac_address[4],
        mac_address[5]
    );

    if (s_ap_netif == NULL)
    {
        s_ap_netif =
            esp_netif_create_default_wifi_ap();

        if (s_ap_netif == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    size_t ap_ssid_length = strlen(s_ap_ssid);

    memcpy(
        ap_config.ap.ssid,
        s_ap_ssid,
        ap_ssid_length
    );

    snprintf(
        (char *)ap_config.ap.password,
        sizeof(ap_config.ap.password),
        "%s",
        PROVISIONING_AP_PASSWORD
    );

    ap_config.ap.ssid_len =
        (uint8_t)ap_ssid_length;

    ap_config.ap.channel =
        PROVISIONING_AP_CHANNEL;

    ap_config.ap.max_connection =
        PROVISIONING_AP_MAX_CLIENTS;

    ap_config.ap.authmode =
        WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_mode(
        WIFI_MODE_APSTA
    );

    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_wifi_set_config(
        WIFI_IF_AP,
        &ap_config
    );

    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_wifi_start();

    if (err != ESP_OK)
    {
        return err;
    }

    http_config.max_uri_handlers = 6;
    http_config.lru_purge_enable = true;

    err = httpd_start(
        &s_http_server,
        &http_config
    );

    if (err != ESP_OK)
    {
        esp_wifi_stop();

        return err;
    }

    err = httpd_register_uri_handler(
        s_http_server,
        &ROOT_URI
    );

    if (err != ESP_OK)
    {
        goto http_error;
    }

    err = httpd_register_uri_handler(
        s_http_server,
        &SCAN_URI
    );

    if (err != ESP_OK)
    {
        goto http_error;
    }

    err = httpd_register_uri_handler(
        s_http_server,
        &CONFIG_URI
    );

    if (err != ESP_OK)
    {
        goto http_error;
    }

    s_restart_scheduled = false;
    s_running = true;

    ESP_LOGI(TAG, "Provisioning mode started");
    ESP_LOGI(TAG, "SoftAP SSID: %s", s_ap_ssid);
    ESP_LOGI(
        TAG,
        "SoftAP password: %s",
        PROVISIONING_AP_PASSWORD
    );
    ESP_LOGI(
        TAG,
        "Open http://192.168.4.1"
    );

    return ESP_OK;

http_error:
    httpd_stop(s_http_server);
    s_http_server = NULL;

    esp_wifi_stop();

    return err;
}

esp_err_t provisioning_stop(void)
{
    esp_err_t result = ESP_OK;

    if (!s_running)
    {
        return ESP_OK;
    }

    if (s_http_server != NULL)
    {
        esp_err_t err =
            httpd_stop(s_http_server);

        if (err != ESP_OK)
        {
            result = err;
        }

        s_http_server = NULL;
    }

    esp_err_t wifi_err = esp_wifi_stop();

    if ((wifi_err != ESP_OK) &&
        (result == ESP_OK))
    {
        result = wifi_err;
    }

    s_running = false;
    s_restart_scheduled = false;

    ESP_LOGI(TAG, "Provisioning mode stopped");

    return result;
}

bool provisioning_is_running(void)
{
    return s_running;
}

const char *provisioning_get_ap_ssid(void)
{
    return s_ap_ssid;
}

const char *provisioning_get_ap_password(void)
{
    return PROVISIONING_AP_PASSWORD;
}