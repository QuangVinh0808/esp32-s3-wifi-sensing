#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "config_store.h"
#include "wifi_manager.h"

static const char *TAG = "M2_TEST";

/*
 * Thời gian tối đa chờ ESP32 kết nối router và nhận IP.
 */
#define M2_CONNECTION_TIMEOUT_MS    30000

/*
 * Chu kỳ kiểm tra RSSI sau khi đã kết nối.
 */
#define M2_STATUS_INTERVAL_MS        5000

static void print_saved_config(
    const app_config_t *config
)
{
    if (config == NULL)
    {
        return;
    }

    ESP_LOGI(TAG, "----- SAVED CONFIGURATION -----");

    ESP_LOGI(
        TAG,
        "Config version  : %u",
        config->version
    );

    ESP_LOGI(
        TAG,
        "SSID            : %s",
        config->ssid
    );

    /*
     * Không in password ra Serial Monitor.
     */
    ESP_LOGI(
        TAG,
        "Password        : %s",
        config->password[0] != '\0'
            ? "[configured]"
            : "[open network]"
    );

    ESP_LOGI(
        TAG,
        "Provisioned     : %s",
        config->provisioned
            ? "true"
            : "false"
    );

    ESP_LOGI(
        TAG,
        "Packet rate     : %u Hz",
        config->packet_rate_hz
    );

    ESP_LOGI(
        TAG,
        "Motion threshold: %.3f",
        config->motion_threshold
    );
}

static void print_access_point_info(void)
{
    wifi_ap_record_t ap_info;
    esp_err_t err;

    err = wifi_manager_get_ap_info(&ap_info);

    if (err != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Cannot get AP information: %s",
            esp_err_to_name(err)
        );

        return;
    }

    ESP_LOGI(TAG, "----- ACCESS POINT STATUS -----");

    /*
     * SSID trong wifi_ap_record_t có kích thước cố định.
     * Dùng precision để tránh đọc vượt quá buffer.
     */
    ESP_LOGI(
        TAG,
        "SSID     : %.*s",
        (int)sizeof(ap_info.ssid),
        (const char *)ap_info.ssid
    );

    ESP_LOGI(
        TAG,
        "RSSI     : %d dBm",
        ap_info.rssi
    );

    ESP_LOGI(
        TAG,
        "Channel  : %u",
        ap_info.primary
    );

    ESP_LOGI(
        TAG,
        "Auth mode: %d",
        (int)ap_info.authmode
    );

    ESP_LOGI(
        TAG,
        "BSSID    : %02X:%02X:%02X:%02X:%02X:%02X",
        ap_info.bssid[0],
        ap_info.bssid[1],
        ap_info.bssid[2],
        ap_info.bssid[3],
        ap_info.bssid[4],
        ap_info.bssid[5]
    );
}

void app_main(void)
{
    app_config_t config;
    esp_err_t err;

    ESP_LOGI(TAG, "Starting M2 Wi-Fi station test");

    /*
     * Bước 1: Khởi tạo NVS.
     */
    err = config_store_init();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot initialize config store: %s",
            esp_err_to_name(err)
        );

        return;
    }

    /*
     * Bước 2: Đọc SSID/password từ NVS.
     */
    config_store_set_defaults(&config);

    err = config_store_load(&config);

    if (err == ESP_ERR_NOT_FOUND)
    {
        ESP_LOGE(
            TAG,
            "No Wi-Fi configuration found in NVS"
        );

        ESP_LOGE(
            TAG,
            "Run M1 once with your real 2.4 GHz Wi-Fi credentials"
        );

        return;
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot load configuration: %s",
            esp_err_to_name(err)
        );

        return;
    }

    /*
     * Bước 3: Kiểm tra credentials.
     */
    if (!config_store_has_credentials(&config))
    {
        ESP_LOGE(
            TAG,
            "Stored configuration has no valid credentials"
        );

        return;
    }

    print_saved_config(&config);

    /*
     * Bước 4: Khởi tạo Wi-Fi manager.
     */
    err = wifi_manager_init();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot initialize Wi-Fi manager: %s",
            esp_err_to_name(err)
        );

        return;
    }

    /*
     * Bước 5: Bắt đầu Station mode.
     */
    err = wifi_manager_start(&config);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot start Wi-Fi manager: %s",
            esp_err_to_name(err)
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "Waiting up to %d seconds for connection",
        M2_CONNECTION_TIMEOUT_MS / 1000
    );

    /*
     * Bước 6: Chờ GOT_IP hoặc FAILED_BIT.
     */
    err = wifi_manager_wait_connected(
        pdMS_TO_TICKS(M2_CONNECTION_TIMEOUT_MS)
    );

    if (err == ESP_ERR_TIMEOUT)
    {
        ESP_LOGE(
            TAG,
            "Connection timeout, current state: %s",
            wifi_manager_state_to_string(
                wifi_manager_get_state()
            )
        );

        wifi_manager_stop();

        return;
    }

    if (err == ESP_FAIL)
    {
        ESP_LOGE(
            TAG,
            "Cannot connect after maximum retries"
        );

        ESP_LOGE(
            TAG,
            "Current state: %s",
            wifi_manager_state_to_string(
                wifi_manager_get_state()
            )
        );

        wifi_manager_stop();

        return;
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Unexpected connection error: %s",
            esp_err_to_name(err)
        );

        wifi_manager_stop();

        return;
    }

    /*
     * Đến đây ESP32 đã kết nối router và nhận IP.
     */
    ESP_LOGI(TAG, "Wi-Fi connection successful");
    ESP_LOGI(TAG, "M2 initial connection test passed");

    print_access_point_info();

    /*
     * Theo dõi trạng thái và RSSI liên tục.
     *
     * Có thể tắt router để kiểm tra disconnect/retry.
     */
    while (true)
    {
        vTaskDelay(
            pdMS_TO_TICKS(M2_STATUS_INTERVAL_MS)
        );

        if (wifi_manager_is_connected())
        {
            ESP_LOGI(
                TAG,
                "Wi-Fi state: %s",
                wifi_manager_state_to_string(
                    wifi_manager_get_state()
                )
            );

            print_access_point_info();
        }
        else
        {
            ESP_LOGW(
                TAG,
                "Wi-Fi is not connected, state: %s",
                wifi_manager_state_to_string(
                    wifi_manager_get_state()
                )
            );
        }
    }
}