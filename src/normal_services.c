#include "normal_services.h"

#include "csi_capture.h"
#include "csi_processor.h"
#include "csi_traffic.h"
#include "web_server.h"

#include "esp_log.h"

static const char *TAG = "NORMAL_SERVICES";
static bool s_running = false;

static void remember_first_error(esp_err_t err, esp_err_t *first_error)
{
    if ((err != ESP_OK) && (*first_error == ESP_OK))
    {
        *first_error = err;
    }
}

esp_err_t normal_services_start(uint16_t csi_packet_rate_hz)
{
    QueueHandle_t raw_queue;
    QueueHandle_t processed_queue;
    esp_err_t err;

    if (s_running)
    {
        return ESP_OK;
    }

    err = csi_capture_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Cannot start CSI capture: %s", esp_err_to_name(err));
        return err;
    }

    raw_queue = csi_capture_get_queue();
    if (raw_queue == NULL)
    {
        (void)csi_capture_stop();
        return ESP_ERR_INVALID_STATE;
    }

    err = csi_processor_start(raw_queue);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Cannot start CSI processor: %s", esp_err_to_name(err));
        (void)csi_capture_stop();
        return err;
    }

    processed_queue = csi_processor_get_queue();
    if (processed_queue == NULL)
    {
        (void)csi_capture_stop();
        (void)csi_processor_stop();
        return ESP_ERR_INVALID_STATE;
    }

    err = web_server_start(processed_queue);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Cannot start dashboard: %s", esp_err_to_name(err));
        (void)csi_capture_stop();
        (void)csi_processor_stop();
        return err;
    }

    err = csi_traffic_start(csi_packet_rate_hz);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Cannot start CSI traffic: %s", esp_err_to_name(err));
        (void)csi_capture_stop();
        (void)csi_processor_stop();
        (void)web_server_stop();
        return err;
    }

    s_running = true;
    ESP_LOGI(TAG, "M5 normal-mode services started");
    return ESP_OK;
}

esp_err_t normal_services_stop(void)
{
    esp_err_t first_error = ESP_OK;

    if (!s_running)
    {
        return ESP_OK;
    }

    remember_first_error(csi_traffic_stop(), &first_error);
    remember_first_error(csi_capture_stop(), &first_error);
    remember_first_error(csi_processor_stop(), &first_error);
    remember_first_error(web_server_stop(), &first_error);

    s_running = false;

    if (first_error == ESP_OK)
    {
        ESP_LOGI(TAG, "M5 normal-mode services stopped");
    }
    else
    {
        ESP_LOGE(TAG, "One or more M5 services failed to stop: %s", esp_err_to_name(first_error));
    }

    return first_error;
}

bool normal_services_is_running(void)
{
    return s_running;
}