#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "app_config.h"
#include "config_store.h"

static const char *TAG = "M1_TEST";

/*
 * Đặt bằng 1 để kiểm tra chức năng xóa.
 * Bình thường phải để bằng 0.
 */
#define M1_ERASE_CONFIG_ON_BOOT    1

/*
 * Tạo cấu hình demo nếu chưa có dữ liệu trong NVS.
 */
#define M1_CREATE_DEMO_CONFIG      0

static void print_config(const app_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    ESP_LOGI(TAG, "----- STORED CONFIGURATION -----");

    ESP_LOGI(
        TAG,
        "Version         : %u",
        config->version
    );

    ESP_LOGI(
        TAG,
        "SSID            : %s",
        config->ssid
    );

    ESP_LOGI(
        TAG,
        "Password length : %u",
        (unsigned int)strlen(config->password)
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

    ESP_LOGI(
        TAG,
        "Provisioned     : %s",
        config->provisioned ? "true" : "false"
    );

    ESP_LOGI(
        TAG,
        "Has credentials : %s",
        config_store_has_credentials(config)
            ? "yes"
            : "no"
    );
}

static esp_err_t create_demo_config(void)
{
    app_config_t config;

    config_store_set_defaults(&config);

    /*
     * Hiện tại đây chỉ là thông tin Wi-Fi giả
     * dùng để kiểm tra chức năng NVS.
     */
    snprintf(
        config.ssid,
        sizeof(config.ssid),
        "%s",
        "TestNetwork"
    );

    snprintf(
        config.password,
        sizeof(config.password),
        "%s",
        "TestPassword123"
    );

    config.packet_rate_hz = 20;
    config.motion_threshold = 3.5f;
    config.provisioned = true;

    ESP_LOGI(TAG, "Saving demo configuration");

    return config_store_save(&config);
}

void app_main(void)
{
    app_config_t config;
    esp_err_t err;

    ESP_LOGI(
        TAG,
        "Starting M1 NVS configuration test"
    );

    err = config_store_init();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot initialize NVS: %s",
            esp_err_to_name(err)
        );

        return;
    }

#if M1_ERASE_CONFIG_ON_BOOT

    err = config_store_erase();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot erase configuration: %s",
            esp_err_to_name(err)
        );

        return;
    }

#endif

    config_store_set_defaults(&config);

    err = config_store_load(&config);
    if (err == ESP_ERR_NOT_FOUND)
    {
        ESP_LOGW(
            TAG,
            "First boot: no configuration found"
        );

#if M1_CREATE_DEMO_CONFIG

        err = create_demo_config();

        if (err != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "Cannot save demo configuration: %s",
                esp_err_to_name(err)
            );

            return;
        }

        /*
         * Xóa giá trị hiện tại trong RAM rồi đọc lại
         * từ flash để kiểm tra thao tác save/load.
         */
        config_store_set_defaults(&config);

        err = config_store_load(&config);

        if (err != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "Cannot verify saved config: %s",
                esp_err_to_name(err)
            );

            return;
        }

        ESP_LOGI(
            TAG,
            "Write/read verification successful"
        );

        print_config(&config);

        ESP_LOGI(
            TAG,
            "Press RST to verify persistence"
        );

#else

        ESP_LOGI(
            TAG,
            "Demo config creation is disabled"
        );

#endif
    }
    else if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to load config: %s",
            esp_err_to_name(err)
        );

        return;
    }
    else
    {
        ESP_LOGI(
            TAG,
            "Configuration survived reset"
        );

        print_config(&config);

        ESP_LOGI(TAG, "M1 test passed");
    }
}