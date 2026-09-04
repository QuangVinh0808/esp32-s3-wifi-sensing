#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdbool.h>

#include "esp_err.h"
#include "app_config.h"

/**
 * @brief Khởi tạo partition NVS mặc định.
 */
esp_err_t config_store_init(void);

/**
 * @brief Gán giá trị mặc định cho cấu hình.
 */
void config_store_set_defaults(app_config_t *config);

/**
 * @brief Kiểm tra dữ liệu cấu hình có hợp lệ hay không.
 */
bool config_store_validate(const app_config_t *config);

/**
 * @brief Đọc cấu hình từ NVS.
 *
 * @return
 * - ESP_OK: đọc thành công.
 * - ESP_ERR_NVS_NOT_FOUND: chưa có cấu hình.
 * - Mã lỗi khác: dữ liệu hoặc NVS có lỗi.
 */
esp_err_t config_store_load(app_config_t *config);

/**
 * @brief Ghi cấu hình vào NVS.
 */
esp_err_t config_store_save(const app_config_t *config);

/**
 * @brief Xóa toàn bộ cấu hình trong namespace app_cfg.
 */
esp_err_t config_store_erase(void);

/**
 * @brief Kiểm tra cấu hình đã có Wi-Fi credentials hay chưa.
 */
bool config_store_has_credentials(const app_config_t *config);

//Active config cũ chưa bị ghi đè
esp_err_t config_store_save_credentials(
    const app_config_t *config
);

esp_err_t config_store_save_pending(
    const app_config_t *config
);
//Đọc pending config
esp_err_t config_store_load_pending(
    app_config_t *config
);

/**
 * @brief Xóa pending config.
 */
esp_err_t config_store_erase_pending(void);

/**
 * @brief Chuyển pending config thành active config.
 */
esp_err_t config_store_promote_pending(void);
#endif /* CONFIG_STORE_H */