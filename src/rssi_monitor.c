#include "rssi_monitor.h"

#include <stddef.h>

#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "freertos/task.h"

static const char *TAG = "RSSI_MONITOR";

#define RSSI_MONITOR_QUEUE_LENGTH       1U
#define RSSI_MONITOR_TASK_STACK_SIZE    3072U
#define RSSI_MONITOR_TASK_PRIORITY      4U
#define RSSI_MONITOR_MIN_RATE_HZ        1U
#define RSSI_MONITOR_MAX_RATE_HZ        20U
#define RSSI_MONITOR_STOP_WAIT_MS       1000U

static QueueHandle_t s_sample_queue = NULL;
static TaskHandle_t s_monitor_task = NULL;

static volatile bool s_running = false;
static uint16_t s_sample_rate_hz = 10U;

static void rssi_monitor_task(void *argument)
{
    (void)argument;

    TickType_t last_wake_time = xTaskGetTickCount();
    TickType_t sample_period = pdMS_TO_TICKS(
        1000U / s_sample_rate_hz
    );

    uint32_t sequence = 0U;

    if (sample_period == 0U)
    {
        sample_period = 1U;
    }

    while (s_running)
    {
        wifi_ap_record_t ap_info = {0};

        if (wifi_manager_is_connected() &&
            (wifi_manager_get_ap_info(&ap_info) == ESP_OK))
        {
            rssi_sample_t sample =
            {
                .timestamp_ms = esp_timer_get_time() / 1000LL,
                .sequence = sequence++,
                .rssi = ap_info.rssi,
                .channel = ap_info.primary
            };

            (void)xQueueOverwrite(
                s_sample_queue,
                &sample
            );
        }

        vTaskDelayUntil(
            &last_wake_time,
            sample_period
        );
    }

    s_monitor_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t rssi_monitor_start(uint16_t sample_rate_hz)
{
    BaseType_t task_result;

    if (s_running)
    {
        return ESP_OK;
    }

    if (!wifi_manager_is_connected())
    {
        ESP_LOGE(TAG, "Wi-Fi is not connected");
        return ESP_ERR_INVALID_STATE;
    }

    if ((sample_rate_hz < RSSI_MONITOR_MIN_RATE_HZ) ||
        (sample_rate_hz > RSSI_MONITOR_MAX_RATE_HZ))
    {
        ESP_LOGE(
            TAG,
            "Invalid sample rate: %u Hz",
            (unsigned int)sample_rate_hz
        );

        return ESP_ERR_INVALID_ARG;
    }

    if (s_sample_queue == NULL)
    {
        s_sample_queue = xQueueCreate(
            RSSI_MONITOR_QUEUE_LENGTH,
            sizeof(rssi_sample_t)
        );

        if (s_sample_queue == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    else
    {
        (void)xQueueReset(s_sample_queue);
    }

    s_sample_rate_hz = sample_rate_hz;
    s_running = true;

    task_result = xTaskCreate(
        rssi_monitor_task,
        "rssi_monitor",
        RSSI_MONITOR_TASK_STACK_SIZE,
        NULL,
        RSSI_MONITOR_TASK_PRIORITY,
        &s_monitor_task
    );

    if (task_result != pdPASS)
    {
        s_running = false;
        s_monitor_task = NULL;

        vQueueDelete(s_sample_queue);
        s_sample_queue = NULL;

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "RSSI monitor started at %u Hz",
        (unsigned int)s_sample_rate_hz
    );

    return ESP_OK;
}

esp_err_t rssi_monitor_stop(void)
{
    uint32_t elapsed_ms = 0U;

    if (!s_running && (s_monitor_task == NULL))
    {
        if (s_sample_queue != NULL)
        {
            vQueueDelete(s_sample_queue);
            s_sample_queue = NULL;
        }

        return ESP_OK;
    }

    s_running = false;

    while ((s_monitor_task != NULL) &&
           (elapsed_ms < RSSI_MONITOR_STOP_WAIT_MS))
    {
        vTaskDelay(pdMS_TO_TICKS(10U));
        elapsed_ms += 10U;
    }

    if (s_monitor_task != NULL)
    {
        ESP_LOGE(TAG, "RSSI monitor task did not stop");
        return ESP_ERR_TIMEOUT;
    }

    if (s_sample_queue != NULL)
    {
        vQueueDelete(s_sample_queue);
        s_sample_queue = NULL;
    }

    ESP_LOGI(TAG, "RSSI monitor stopped");

    return ESP_OK;
}

bool rssi_monitor_is_running(void)
{
    return s_running;
}

QueueHandle_t rssi_monitor_get_queue(void)
{
    return s_sample_queue;
}
