#include "normal_services.h"

#include "rssi_monitor.h"
#include "web_server.h"

#include "esp_log.h"

static const char *TAG = "NORMAL_SERVICES";

static bool s_running = false;

esp_err_t normal_services_start(uint16_t rssi_sample_rate_hz)
{
    QueueHandle_t sample_queue;
    esp_err_t err;

    if (s_running)
    {
        return ESP_OK;
    }

    err = rssi_monitor_start(rssi_sample_rate_hz);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot start RSSI monitor: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    sample_queue = rssi_monitor_get_queue();

    if (sample_queue == NULL)
    {
        (void)rssi_monitor_stop();
        return ESP_ERR_INVALID_STATE;
    }

    err = web_server_start(sample_queue);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot start dashboard server: %s",
            esp_err_to_name(err)
        );

        (void)rssi_monitor_stop();
        return err;
    }

    s_running = true;

    ESP_LOGI(TAG, "Normal-mode services started");

    return ESP_OK;
}

esp_err_t normal_services_stop(void)
{
    esp_err_t err;

    if (!s_running)
    {
        return ESP_OK;
    }

    err = web_server_stop();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot stop dashboard server: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err = rssi_monitor_stop();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot stop RSSI monitor: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    s_running = false;

    ESP_LOGI(TAG, "Normal-mode services stopped");

    return ESP_OK;
}

bool normal_services_is_running(void)
{
    return s_running;
}
