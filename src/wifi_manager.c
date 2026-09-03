#include "wifi_manager.h"

#include <stddef.h>
#include <string.h>

#include "config_store.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "WIFI_MANAGER";

/*
 * Event Group bits.
 */
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAILED_BIT       BIT1

/*
 * Số lần thử kết nối lại tối đa.
 */
#define WIFI_MAXIMUM_RETRY    5

static EventGroupHandle_t s_wifi_event_group = NULL;

static esp_netif_t *s_station_netif = NULL;

static esp_event_handler_instance_t s_wifi_event_instance = NULL;
static esp_event_handler_instance_t s_ip_event_instance = NULL;

static volatile wifi_manager_state_t s_wifi_state =
    WIFI_MANAGER_STATE_UNINITIALIZED;

static bool s_initialized = false;
static bool s_started = false;
static bool s_manual_stop = false;

static int s_retry_count = 0;

/**
 * @brief Đánh dấu kết nối thất bại.
 */
static void wifi_manager_set_failed(void)
{
    s_wifi_state = WIFI_MANAGER_STATE_FAILED;

    if (s_wifi_event_group != NULL)
    {
        xEventGroupClearBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT
        );

        xEventGroupSetBits(
            s_wifi_event_group,
            WIFI_FAILED_BIT
        );
    }
}

/**
 * @brief Xử lý Wi-Fi event và IP event.
 */
static void wifi_manager_event_handler(
    void *handler_arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)handler_arg;

    /*
     * Wi-Fi driver đã khởi động Station.
     */
    if ((event_base == WIFI_EVENT) &&
        (event_id == WIFI_EVENT_STA_START))
    {
        ESP_LOGI(TAG, "Wi-Fi station started");

        s_wifi_state = WIFI_MANAGER_STATE_CONNECTING;
        s_retry_count = 0;

        esp_err_t err = esp_wifi_connect();

        if (err != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "esp_wifi_connect failed: %s",
                esp_err_to_name(err)
            );

            wifi_manager_set_failed();
        }

        return;
    }

    if ((event_base == WIFI_EVENT) &&
        (event_id == WIFI_EVENT_STA_CONNECTED))
    {
        ESP_LOGI(
            TAG,
            "Associated with access point, waiting for IP"
        );

        return;
    }

    // Station bị mất kết nối hoặc kết nối thất bại.
    if ((event_base == WIFI_EVENT) &&
        (event_id == WIFI_EVENT_STA_DISCONNECTED))
    {
        wifi_event_sta_disconnected_t *disconnected_event =
            (wifi_event_sta_disconnected_t *)event_data;

        if (s_wifi_event_group != NULL)
        {
            xEventGroupClearBits(
                s_wifi_event_group,
                WIFI_CONNECTED_BIT
            );
        }

        s_wifi_state = WIFI_MANAGER_STATE_DISCONNECTED;

        if (disconnected_event != NULL)
        {
            ESP_LOGW(
                TAG,
                "Disconnected from AP, reason=%u",
                disconnected_event->reason
            );
        }
        else
        {
            ESP_LOGW(TAG, "Disconnected from AP");
        }

        /*
         * Nếu wifi_manager_stop() chủ động dừng Wi-Fi
         * thì không tự động reconnect.
         */
        if (s_manual_stop)
        {
            ESP_LOGI(
                TAG,
                "Manual stop requested, reconnect disabled"
            );

            return;
        }

        if (s_retry_count < WIFI_MAXIMUM_RETRY)
        {
            s_retry_count++;

            ESP_LOGI(
                TAG,
                "Retrying connection (%d/%d)",
                s_retry_count,
                WIFI_MAXIMUM_RETRY
            );

            s_wifi_state = WIFI_MANAGER_STATE_CONNECTING;

            esp_err_t err = esp_wifi_connect();

            if (err != ESP_OK)
            {
                ESP_LOGE(
                    TAG,
                    "Retry failed to start: %s",
                    esp_err_to_name(err)
                );

                wifi_manager_set_failed();
            }
        }
        else
        {
            ESP_LOGE(
                TAG,
                "Maximum retry count reached"
            );

            wifi_manager_set_failed();
        }

        return;
    }

    /*
     * DHCP client đã nhận được địa chỉ IP.
     * Đây mới được coi là kết nối hoàn chỉnh.
     */
    if ((event_base == IP_EVENT) &&
        (event_id == IP_EVENT_STA_GOT_IP))
    {
        ip_event_got_ip_t *got_ip_event =
            (ip_event_got_ip_t *)event_data;

        s_retry_count = 0;
        s_wifi_state = WIFI_MANAGER_STATE_CONNECTED;

        if (s_wifi_event_group != NULL)
        {
            xEventGroupClearBits(
                s_wifi_event_group,
                WIFI_FAILED_BIT
            );

            xEventGroupSetBits(
                s_wifi_event_group,
                WIFI_CONNECTED_BIT
            );
        }

        if (got_ip_event != NULL)
        {
            ESP_LOGI(
                TAG,
                "Got IP address: " IPSTR,
                IP2STR(&got_ip_event->ip_info.ip)
            );

            ESP_LOGI(
                TAG,
                "Gateway: " IPSTR,
                IP2STR(&got_ip_event->ip_info.gw)
            );

            ESP_LOGI(
                TAG,
                "Netmask: " IPSTR,
                IP2STR(&got_ip_event->ip_info.netmask)
            );
        }

        return;
    }

    /*
     * DHCP mất địa chỉ IP.
     */
    if ((event_base == IP_EVENT) &&
        (event_id == IP_EVENT_STA_LOST_IP))
    {
        ESP_LOGW(TAG, "Station lost IP address");

        if (s_wifi_event_group != NULL)
        {
            xEventGroupClearBits(
                s_wifi_event_group,
                WIFI_CONNECTED_BIT
            );
        }

        if (!s_manual_stop)
        {
            s_wifi_state =
                WIFI_MANAGER_STATE_DISCONNECTED;
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    esp_err_t err;

    wifi_init_config_t wifi_init_config =
        WIFI_INIT_CONFIG_DEFAULT();

    if (s_initialized)
    {
        ESP_LOGW(
            TAG,
            "Wi-Fi manager already initialized"
        );

        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "Initializing Wi-Fi station manager"
    );

    s_wifi_event_group = xEventGroupCreate();

    if (s_wifi_event_group == NULL)
    {
        ESP_LOGE(
            TAG,
            "Failed to create Wi-Fi event group"
        );

        return ESP_ERR_NO_MEM;
    }

    /*
     * Khởi tạo TCP/IP stack.
     */
    err = esp_netif_init();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "esp_netif_init failed: %s",
            esp_err_to_name(err)
        );

        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;

        return err;
    }

    /*
     * Tạo default event loop.
     */
    err = esp_event_loop_create_default();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot create default event loop: %s",
            esp_err_to_name(err)
        );

        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;

        return err;
    }

    /*
     * Tạo network interface cho Station.
     * Interface này chứa DHCP client.
     */
    s_station_netif =
        esp_netif_create_default_wifi_sta();

    if (s_station_netif == NULL)
    {
        ESP_LOGE(
            TAG,
            "Failed to create default Wi-Fi STA interface"
        );

        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;

        return ESP_ERR_NO_MEM;
    }

    /*
     * Luôn dùng WIFI_INIT_CONFIG_DEFAULT().
     */
    //wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();

    err = esp_wifi_init(&wifi_init_config);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "esp_wifi_init failed: %s",
            esp_err_to_name(err)
        );

        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;

        return err;
    }

    /*
     * Project tự quản lý SSID/password bằng config_store.
     * Wi-Fi driver chỉ giữ cấu hình trong RAM.
     */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "esp_wifi_set_storage failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    /*
     * Đăng ký toàn bộ WIFI_EVENT.
     */
    err = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_manager_event_handler,
        NULL,
        &s_wifi_event_instance
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot register Wi-Fi event handler: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    /*
     * Đăng ký toàn bộ IP_EVENT.
     */
    err = esp_event_handler_instance_register(
        IP_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_manager_event_handler,
        NULL,
        &s_ip_event_instance
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot register IP event handler: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    s_retry_count = 0;
    s_manual_stop = false;
    s_started = false;
    s_initialized = true;

    s_wifi_state = WIFI_MANAGER_STATE_INITIALIZED;

    ESP_LOGI(TAG, "Wi-Fi manager initialized");

    return ESP_OK;
}

esp_err_t wifi_manager_start(
    const app_config_t *config
)
{
    esp_err_t err;

    wifi_config_t wifi_config = {0};

    size_t ssid_length;
    size_t password_length;

    if (!s_initialized)
    {
        ESP_LOGE(TAG, "Wi-Fi manager is not initialized");

        return ESP_ERR_INVALID_STATE;
    }

    if (s_started)
    {
        ESP_LOGW(TAG, "Wi-Fi station is already started");

        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!config_store_validate(config))
    {
        ESP_LOGE(TAG, "Invalid application configuration");

        return ESP_ERR_INVALID_ARG;
    }

    if (!config_store_has_credentials(config))
    {
        ESP_LOGE(TAG, "Wi-Fi credentials are missing");

        return ESP_ERR_INVALID_ARG;
    }

    ssid_length = strnlen(
        config->ssid,
        sizeof(config->ssid)
    );

    password_length = strnlen(
        config->password,
        sizeof(config->password)
    );

    if ((ssid_length == 0) ||
        (ssid_length > sizeof(wifi_config.sta.ssid)))
    {
        ESP_LOGE(TAG, "Invalid SSID length");

        return ESP_ERR_INVALID_ARG;
    }

    if (password_length >
        sizeof(wifi_config.sta.password))
    {
        ESP_LOGE(TAG, "Invalid password length");

        return ESP_ERR_INVALID_ARG;
    }

    /*
     * wifi_config đã được khởi tạo bằng 0.
     * Không dùng strcpy() vì SSID/password buffer có kích thước cố định.
     */
    memcpy(
        wifi_config.sta.ssid,
        config->ssid,
        ssid_length
    );

    if (password_length > 0)
    {
        memcpy(
            wifi_config.sta.password,
            config->password,
            password_length
        );
    }

    /*
     * Nếu password rỗng thì cho phép mạng open.
     * Nếu có password thì yêu cầu tối thiểu WPA2.
     */
    if (password_length == 0)
    {
        wifi_config.sta.threshold.authmode =
            WIFI_AUTH_OPEN;
    }
    else
    {
        wifi_config.sta.threshold.authmode =
            WIFI_AUTH_WPA2_PSK;
    }

    /*
     * Protected Management Frames.
     * capable=true nhưng required=false để tương thích
     * với nhiều router hơn.
     */
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    /*
     * Driver không tự retry; application quản lý retry
     * trong WIFI_EVENT_STA_DISCONNECTED.
     */
    wifi_config.sta.failure_retry_cnt = 0;

    xEventGroupClearBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT
    );

    s_retry_count = 0;
    s_manual_stop = false;
    s_wifi_state = WIFI_MANAGER_STATE_CONNECTING;

    err = esp_wifi_set_mode(WIFI_MODE_STA);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "esp_wifi_set_mode failed: %s",
            esp_err_to_name(err)
        );

        s_wifi_state = WIFI_MANAGER_STATE_FAILED;

        return err;
    }

    err = esp_wifi_set_config(
        WIFI_IF_STA,
        &wifi_config
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "esp_wifi_set_config failed: %s",
            esp_err_to_name(err)
        );

        s_wifi_state = WIFI_MANAGER_STATE_FAILED;

        return err;
    }

    ESP_LOGI(
        TAG,
        "Starting connection to SSID: %s",
        config->ssid
    );

    /*
     * Không log password.
     */
    err = esp_wifi_start();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "esp_wifi_start failed: %s",
            esp_err_to_name(err)
        );

        s_wifi_state = WIFI_MANAGER_STATE_FAILED;

        return err;
    }

    s_started = true;

    /*
     * Tắt power-save để chuẩn bị cho Wi-Fi sensing/CSI.
     * Nếu thất bại, chỉ cảnh báo vì không ảnh hưởng
     * tới chức năng kết nối cơ bản.
     */
    err = esp_wifi_set_ps(WIFI_PS_NONE);

    if (err != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Cannot disable Wi-Fi power save: %s",
            esp_err_to_name(err)
        );
    }
    else
    {
        ESP_LOGI(TAG, "Wi-Fi power save disabled");
    }

    return ESP_OK;
}

esp_err_t wifi_manager_wait_connected(
    TickType_t timeout
)
{
    EventBits_t event_bits;

    if (!s_initialized ||
        (s_wifi_event_group == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    event_bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        timeout
    );

    if ((event_bits & WIFI_CONNECTED_BIT) != 0)
    {
        return ESP_OK;
    }

    if ((event_bits & WIFI_FAILED_BIT) != 0)
    {
        return ESP_FAIL;
    }

    return ESP_ERR_TIMEOUT;
}

bool wifi_manager_is_connected(void)
{
    EventBits_t event_bits;

    if (!s_initialized ||
        (s_wifi_event_group == NULL))
    {
        return false;
    }

    event_bits = xEventGroupGetBits(
        s_wifi_event_group
    );

    return (event_bits & WIFI_CONNECTED_BIT) != 0;
}

wifi_manager_state_t wifi_manager_get_state(void)
{
    return s_wifi_state;
}

const char *wifi_manager_state_to_string(
    wifi_manager_state_t state
)
{
    switch (state)
    {
        case WIFI_MANAGER_STATE_UNINITIALIZED:
            return "UNINITIALIZED";

        case WIFI_MANAGER_STATE_INITIALIZED:
            return "INITIALIZED";

        case WIFI_MANAGER_STATE_CONNECTING:
            return "CONNECTING";

        case WIFI_MANAGER_STATE_CONNECTED:
            return "CONNECTED";

        case WIFI_MANAGER_STATE_DISCONNECTED:
            return "DISCONNECTED";

        case WIFI_MANAGER_STATE_FAILED:
            return "FAILED";

        default:
            return "UNKNOWN";
    }
}

esp_err_t wifi_manager_get_ap_info(
    wifi_ap_record_t *ap_info
)
{
    if (ap_info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!wifi_manager_is_connected())
    {
        ESP_LOGW(
            TAG,
            "Cannot get AP info: Wi-Fi is not connected"
        );

        return ESP_ERR_INVALID_STATE;
    }

    memset(ap_info, 0, sizeof(*ap_info));

    return esp_wifi_sta_get_ap_info(ap_info);
}

esp_err_t wifi_manager_stop(void)
{
    esp_err_t err;

    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_started)
    {
        ESP_LOGW(TAG, "Wi-Fi station is already stopped");

        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping Wi-Fi station");

    /*
     * Ngăn event handler tự reconnect khi esp_wifi_stop()
     * tạo ra disconnect event.
     */
    s_manual_stop = true;

    xEventGroupClearBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT
    );

    err = esp_wifi_stop();

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "esp_wifi_stop failed: %s",
            esp_err_to_name(err)
        );

        s_manual_stop = false;

        return err;
    }

    s_started = false;
    s_retry_count = 0;
    s_wifi_state = WIFI_MANAGER_STATE_INITIALIZED;

    ESP_LOGI(TAG, "Wi-Fi station stopped");

    return ESP_OK;
}