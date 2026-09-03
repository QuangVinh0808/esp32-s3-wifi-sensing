#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"

#include "app_config.h"

typedef enum
{
    WIFI_MANAGER_STATE_UNINITIALIZED = 0,
    WIFI_MANAGER_STATE_INITIALIZED,
    WIFI_MANAGER_STATE_CONNECTING,
    WIFI_MANAGER_STATE_CONNECTED,
    WIFI_MANAGER_STATE_DISCONNECTED,
    WIFI_MANAGER_STATE_FAILED
} wifi_manager_state_t;

/**
 * @brief Khởi tạo TCP/IP stack, event loop và Wi-Fi driver.
 *
 * Hàm này chỉ khởi tạo driver, chưa bắt đầu kết nối.
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Bắt đầu Wi-Fi Station bằng cấu hình từ NVS.
 *
 * @param config Cấu hình chứa SSID và password.
 */
esp_err_t wifi_manager_start(
    const app_config_t *config
);

/**
 * @brief Chờ Wi-Fi kết nối thành công hoặc thất bại.
 *
 * @param timeout Thời gian chờ theo FreeRTOS ticks.
 *
 * @return
 * - ESP_OK: Đã nhận được địa chỉ IP.
 * - ESP_FAIL: Đã thử quá số lần cho phép.
 * - ESP_ERR_TIMEOUT: Hết thời gian chờ.
 */
esp_err_t wifi_manager_wait_connected(
    TickType_t timeout
);

/**
 * @brief Kiểm tra ESP32 đã nhận IP hay chưa.
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Lấy trạng thái hiện tại của Wi-Fi manager.
 */
wifi_manager_state_t wifi_manager_get_state(void);

/**
 * @brief Chuyển trạng thái sang chuỗi để in log.
 */
const char *wifi_manager_state_to_string(
    wifi_manager_state_t state
);

/**
 * @brief Lấy thông tin router đang kết nối.
 *
 * Có thể lấy RSSI, channel, BSSID và authentication mode.
 */
esp_err_t wifi_manager_get_ap_info(
    wifi_ap_record_t *ap_info
);

/**
 * @brief Dừng Wi-Fi Station.
 *
 * Driver vẫn được giữ ở trạng thái initialized để có thể
 * gọi wifi_manager_start() lại sau này.
 */
esp_err_t wifi_manager_stop(void);

#endif /* WIFI_MANAGER_H */