#ifndef PROVISIONING_H
#define PROVISIONING_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Khởi động SoftAP, scan API và HTTP server.
 *
 * Yêu cầu wifi_manager_init() đã được gọi và
 * Wi-Fi hiện đang ở trạng thái stopped.
 */
esp_err_t provisioning_start(void);

/**
 * @brief Dừng HTTP server và SoftAP.
 */
esp_err_t provisioning_stop(void);

/**
 * @brief Kiểm tra provisioning đang chạy hay không.
 */
bool provisioning_is_running(void);

/**
 * @brief Lấy SSID SoftAP hiện tại.
 */
const char *provisioning_get_ap_ssid(void);

/**
 * @brief Lấy password SoftAP.
 */
const char *provisioning_get_ap_password(void);

#endif /* PROVISIONING_H */