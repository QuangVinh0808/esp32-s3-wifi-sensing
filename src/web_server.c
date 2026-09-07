#include "web_server.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dashboard_page.h"
#include "rssi_monitor.h"
#include "wifi_manager.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "freertos/task.h"

#ifndef CONFIG_HTTPD_WS_SUPPORT
#error "Enable CONFIG_HTTPD_WS_SUPPORT in menuconfig"
#endif

static const char *TAG = "WEB_SERVER";

#define WEB_SERVER_MAX_OPEN_SOCKETS    4U
#define WEB_STREAM_TASK_STACK_SIZE     4096U
#define WEB_STREAM_TASK_PRIORITY       4U
#define WEB_STREAM_STOP_WAIT_MS        1000U
#define WEB_STREAM_JSON_SIZE           160U
#define STATUS_JSON_SIZE               512U
#define ESCAPED_SSID_SIZE              193U

typedef struct
{
    httpd_handle_t server;
    char payload[WEB_STREAM_JSON_SIZE];
} websocket_work_t;

static httpd_handle_t s_server = NULL;
static QueueHandle_t s_sample_queue = NULL;
static TaskHandle_t s_stream_task = NULL;
static volatile bool s_running = false;

static void json_escape_ssid(
    const uint8_t *ssid,
    char *output,
    size_t output_size
)
{
    size_t output_index = 0U;

    if ((ssid == NULL) ||
        (output == NULL) ||
        (output_size == 0U))
    {
        return;
    }

    for (size_t index = 0U;
         (index < 32U) && (ssid[index] != 0U);
         index++)
    {
        const uint8_t character = ssid[index];

        if ((character == '"') || (character == '\\'))
        {
            if ((output_index + 2U) >= output_size)
            {
                break;
            }

            output[output_index++] = '\\';
            output[output_index++] = (char)character;
        }
        else if ((character >= 0x20U) && (character <= 0x7EU))
        {
            if ((output_index + 1U) >= output_size)
            {
                break;
            }

            output[output_index++] = (char)character;
        }
        else
        {
            int written;

            if ((output_index + 6U) >= output_size)
            {
                break;
            }

            written = snprintf(
                &output[output_index],
                output_size - output_index,
                "\\u%04X",
                (unsigned int)character
            );

            if (written != 6)
            {
                break;
            }

            output_index += 6U;
        }
    }

    output[output_index] = '\0';
}

static esp_err_t root_get_handler(httpd_req_t *request)
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
        DASHBOARD_PAGE_HTML,
        HTTPD_RESP_USE_STRLEN
    );
}

static esp_err_t status_get_handler(httpd_req_t *request)
{
    wifi_ap_record_t ap_info = {0};
    esp_netif_ip_info_t ip_info = {0};

    char escaped_ssid[ESCAPED_SSID_SIZE] = {0};
    char response[STATUS_JSON_SIZE] = {0};

    esp_netif_t *station_netif;
    esp_err_t err;
    int written;

    err = wifi_manager_get_ap_info(&ap_info);

    if (err != ESP_OK)
    {
        httpd_resp_set_status(
            request,
            "503 Service Unavailable"
        );

        httpd_resp_set_type(request, HTTPD_TYPE_JSON);

        return httpd_resp_sendstr(
            request,
            "{\"connected\":false}"
        );
    }

    station_netif = esp_netif_get_handle_from_ifkey(
        "WIFI_STA_DEF"
    );

    if (station_netif == NULL)
    {
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "STA netif unavailable"
        );
    }

    err = esp_netif_get_ip_info(
        station_netif,
        &ip_info
    );

    if (err != ESP_OK)
    {
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Cannot read IP information"
        );
    }

    json_escape_ssid(
        ap_info.ssid,
        escaped_ssid,
        sizeof(escaped_ssid)
    );

    written = snprintf(
        response,
        sizeof(response),
        "{\"connected\":true,"
        "\"ssid\":\"%s\","
        "\"rssi\":%d,"
        "\"channel\":%u,"
        "\"ip\":\"" IPSTR "\","
        "\"uptime_ms\":%" PRId64 "}",
        escaped_ssid,
        (int)ap_info.rssi,
        (unsigned int)ap_info.primary,
        IP2STR(&ip_info.ip),
        esp_timer_get_time() / 1000LL
    );

    if ((written < 0) ||
        ((size_t)written >= sizeof(response)))
    {
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Status response too large"
        );
    }

    httpd_resp_set_type(request, HTTPD_TYPE_JSON);

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    return httpd_resp_send(
        request,
        response,
        written
    );
}

static esp_err_t websocket_handler(httpd_req_t *request)
{
    const int socket_fd = httpd_req_to_sockfd(request);

    ESP_LOGI(
        TAG,
        "WebSocket client active on fd=%d",
        socket_fd
    );

    return ESP_OK;
}

static void websocket_broadcast_work(void *argument)
{
    websocket_work_t *work =
        (websocket_work_t *)argument;

    int client_fds[WEB_SERVER_MAX_OPEN_SOCKETS] = {0};
    size_t client_count = WEB_SERVER_MAX_OPEN_SOCKETS;

    if (work == NULL)
    {
        return;
    }

    if (httpd_get_client_list(
            work->server,
            &client_count,
            client_fds
        ) == ESP_OK)
    {
        for (size_t index = 0U;
             index < client_count;
             index++)
        {
            const int socket_fd = client_fds[index];

            if (httpd_ws_get_fd_info(
                    work->server,
                    socket_fd
                ) == HTTPD_WS_CLIENT_WEBSOCKET)
            {
                httpd_ws_frame_t frame =
                {
                    .final = true,
                    .fragmented = false,
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t *)work->payload,
                    .len = strlen(work->payload)
                };

                esp_err_t err = httpd_ws_send_frame_async(
                    work->server,
                    socket_fd,
                    &frame
                );

                if (err != ESP_OK)
                {
                    ESP_LOGW(
                        TAG,
                        "WebSocket send failed on fd=%d: %s",
                        socket_fd,
                        esp_err_to_name(err)
                    );
                }
            }
        }
    }

    free(work);
}

static esp_err_t queue_sample_broadcast(
    const rssi_sample_t *sample
)
{
    websocket_work_t *work;
    int written;
    esp_err_t err;

    if ((sample == NULL) || (s_server == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    work = malloc(sizeof(*work));

    if (work == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    work->server = s_server;

    written = snprintf(
        work->payload,
        sizeof(work->payload),
        "{\"t_ms\":%" PRId64 ","
        "\"seq\":%" PRIu32 ","
        "\"rssi\":%d,"
        "\"channel\":%u}",
        sample->timestamp_ms,
        sample->sequence,
        (int)sample->rssi,
        (unsigned int)sample->channel
    );

    if ((written < 0) ||
        ((size_t)written >= sizeof(work->payload)))
    {
        free(work);
        return ESP_ERR_INVALID_SIZE;
    }

    err = httpd_queue_work(
        s_server,
        websocket_broadcast_work,
        work
    );

    if (err != ESP_OK)
    {
        free(work);
    }

    return err;
}

static void web_stream_task(void *argument)
{
    (void)argument;

    while (s_running)
    {
        rssi_sample_t sample = {0};

        if (xQueueReceive(
                s_sample_queue,
                &sample,
                pdMS_TO_TICKS(100U)
            ) == pdTRUE)
        {
            esp_err_t err = queue_sample_broadcast(&sample);

            if ((err != ESP_OK) &&
                (err != ESP_ERR_NO_MEM))
            {
                ESP_LOGD(
                    TAG,
                    "Sample broadcast skipped: %s",
                    esp_err_to_name(err)
                );
            }
        }
    }

    s_stream_task = NULL;
    vTaskDelete(NULL);
}

static const httpd_uri_t ROOT_URI =
{
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t STATUS_URI =
{
    .uri = "/api/status",
    .method = HTTP_GET,
    .handler = status_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t WEBSOCKET_URI =
{
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = websocket_handler,
    .user_ctx = NULL,
    .is_websocket = true,
    .handle_ws_control_frames = false
};

static esp_err_t register_uri_handlers(void)
{
    esp_err_t err;

    err = httpd_register_uri_handler(
        s_server,
        &ROOT_URI
    );

    if (err != ESP_OK)
    {
        return err;
    }

    err = httpd_register_uri_handler(
        s_server,
        &STATUS_URI
    );

    if (err != ESP_OK)
    {
        return err;
    }

    return httpd_register_uri_handler(
        s_server,
        &WEBSOCKET_URI
    );
}

esp_err_t web_server_start(QueueHandle_t sample_queue)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    BaseType_t task_result;
    esp_err_t err;

    if (s_running)
    {
        return ESP_OK;
    }

    if (sample_queue == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!wifi_manager_is_connected())
    {
        return ESP_ERR_INVALID_STATE;
    }

    config.max_uri_handlers = 6U;
    config.max_open_sockets = WEB_SERVER_MAX_OPEN_SOCKETS;
    config.lru_purge_enable = true;
    config.stack_size = 6144U;

    err = httpd_start(&s_server, &config);

    if (err != ESP_OK)
    {
        s_server = NULL;
        return err;
    }

    err = register_uri_handlers();

    if (err != ESP_OK)
    {
        (void)httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    s_sample_queue = sample_queue;
    s_running = true;

    task_result = xTaskCreate(
        web_stream_task,
        "web_stream",
        WEB_STREAM_TASK_STACK_SIZE,
        NULL,
        WEB_STREAM_TASK_PRIORITY,
        &s_stream_task
    );

    if (task_result != pdPASS)
    {
        s_running = false;
        s_stream_task = NULL;
        s_sample_queue = NULL;

        (void)httpd_stop(s_server);
        s_server = NULL;

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Dashboard server started on port 80");

    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    uint32_t elapsed_ms = 0U;
    esp_err_t err = ESP_OK;

    if (!s_running && (s_server == NULL))
    {
        return ESP_OK;
    }

    s_running = false;

    while ((s_stream_task != NULL) &&
           (elapsed_ms < WEB_STREAM_STOP_WAIT_MS))
    {
        vTaskDelay(pdMS_TO_TICKS(10U));
        elapsed_ms += 10U;
    }

    if (s_stream_task != NULL)
    {
        ESP_LOGE(TAG, "Web stream task did not stop");
        return ESP_ERR_TIMEOUT;
    }

    if (s_server != NULL)
    {
        err = httpd_stop(s_server);
        s_server = NULL;
    }

    s_sample_queue = NULL;

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Dashboard server stopped");
    }

    return err;
}

bool web_server_is_running(void)
{
    return s_running;
}