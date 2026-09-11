#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "config_store.h"
#include "csi_capture.h"
#include "normal_services.h"
#include "provisioning.h"
#include "wifi_manager.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP";

#define CONFIG_BUTTON_GPIO              GPIO_NUM_0
#define CONFIG_BUTTON_HOLD_MS           3000U
#define NORMAL_LOOP_PERIOD_MS           50U
#define NORMAL_STATUS_PERIOD_MS         5000U
#define WIFI_CONNECT_TIMEOUT_MS         30000U
#define M5_MIN_PACKET_RATE_HZ           20U
#define M5_DEFAULT_PACKET_RATE_HZ       20U
#define M5_MAX_PACKET_RATE_HZ           100U

static uint16_t sanitize_packet_rate(uint16_t packet_rate_hz)
{
    if ((packet_rate_hz < M5_MIN_PACKET_RATE_HZ) ||
        (packet_rate_hz > M5_MAX_PACKET_RATE_HZ))
    {
        return M5_DEFAULT_PACKET_RATE_HZ;
    }

    return packet_rate_hz;
}

static void config_button_init(void)
{
    gpio_config_t button_config =
    {
        .pin_bit_mask = 1ULL << CONFIG_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&button_config));
}

static void enter_provisioning_forever(void)
{
    esp_err_t err = provisioning_start();

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Cannot start provisioning: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Provisioning AP: %s", provisioning_get_ap_ssid());
    ESP_LOGI(TAG, "Open http://192.168.4.1");

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

static void stop_normal_mode(void)
{
    esp_err_t err;

    err = normal_services_stop();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Cannot stop normal services: %s", esp_err_to_name(err));
    }

    err = wifi_manager_stop();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Cannot stop Wi-Fi manager: %s", esp_err_to_name(err));
    }
}

static void normal_mode_loop(uint16_t configured_packet_rate_hz)
{
    TickType_t button_pressed_at = 0U;
    TickType_t last_status_at = xTaskGetTickCount();
    const TickType_t button_hold_ticks = pdMS_TO_TICKS(CONFIG_BUTTON_HOLD_MS);
    const uint16_t packet_rate_hz = sanitize_packet_rate(configured_packet_rate_hz);
    esp_err_t err;

    err = normal_services_start(packet_rate_hz);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Cannot start M5 services: %s", esp_err_to_name(err));
        stop_normal_mode();
        enter_provisioning_forever();
        return;
    }

    ESP_LOGI(TAG, "M5 CSI acquisition is ready at %u Hz", (unsigned int)packet_rate_hz);

    while (true)
    {
        const TickType_t now = xTaskGetTickCount();

        if (gpio_get_level(CONFIG_BUTTON_GPIO) == 0)
        {
            if (button_pressed_at == 0U)
            {
                button_pressed_at = now;
            }
            else if ((now - button_pressed_at) >= button_hold_ticks)
            {
                ESP_LOGW(TAG, "Configuration button held for 3 seconds");
                stop_normal_mode();
                enter_provisioning_forever();
                return;
            }
        }
        else
        {
            button_pressed_at = 0U;
        }

        if (wifi_manager_get_state() == WIFI_MANAGER_STATE_FAILED)
        {
            ESP_LOGE(TAG, "Wi-Fi reconnect failed; entering provisioning");
            stop_normal_mode();
            enter_provisioning_forever();
            return;
        }

        if ((now - last_status_at) >= pdMS_TO_TICKS(NORMAL_STATUS_PERIOD_MS))
        {
            csi_capture_stats_t stats = {0};

            csi_capture_get_stats(&stats);

            ESP_LOGI(
                TAG,
                "CSI received=%" PRIu32 ", dropped=%" PRIu32 ", invalid=%" PRIu32,
                stats.received_packets,
                stats.dropped_packets,
                stats.invalid_packets
            );

            last_status_at = now;
        }

        vTaskDelay(pdMS_TO_TICKS(NORMAL_LOOP_PERIOD_MS));
    }
}

static esp_err_t connect_with_config(const app_config_t *config)
{
    esp_err_t err = wifi_manager_start(config);

    if (err != ESP_OK)
    {
        return err;
    }

    return wifi_manager_wait_connected(pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
}

void app_main(void)
{
    app_config_t config;
    esp_err_t err;

    ESP_LOGI(TAG, "Starting ESP32-S3 Wi-Fi sensing application");

    ESP_ERROR_CHECK(config_store_init());
    ESP_ERROR_CHECK(wifi_manager_init());

    config_button_init();

    err = config_store_load_pending(&config);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Testing pending Wi-Fi configuration: %s", config.ssid);

        err = connect_with_config(&config);

        if (err == ESP_OK)
        {
            err = config_store_promote_pending();

            if (err == ESP_OK)
            {
                ESP_LOGI(TAG, "Pending configuration promoted");
                normal_mode_loop(config.packet_rate_hz);
                return;
            }

            ESP_LOGE(TAG, "Cannot promote pending configuration: %s", esp_err_to_name(err));
        }
        else
        {
            ESP_LOGE(TAG, "Pending configuration failed: %s", esp_err_to_name(err));
        }

        (void)wifi_manager_stop();
        (void)config_store_erase_pending();
        enter_provisioning_forever();
        return;
    }

    if (err != ESP_ERR_NOT_FOUND)
    {
        ESP_LOGE(TAG, "Cannot load pending configuration: %s", esp_err_to_name(err));
        enter_provisioning_forever();
        return;
    }

    err = config_store_load(&config);

    if (err == ESP_ERR_NOT_FOUND)
    {
        ESP_LOGW(TAG, "No active configuration; entering provisioning");
        enter_provisioning_forever();
        return;
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Cannot load active configuration: %s", esp_err_to_name(err));
        enter_provisioning_forever();
        return;
    }

    err = connect_with_config(&config);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Connected using active configuration");
        normal_mode_loop(config.packet_rate_hz);
        return;
    }

    ESP_LOGE(TAG, "Active Wi-Fi connection failed: %s", esp_err_to_name(err));
    (void)wifi_manager_stop();
    enter_provisioning_forever();
}
