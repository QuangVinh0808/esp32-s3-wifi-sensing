#include "config_store.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "CONFIG_STORE";

/*
 * Namespace và key NVS có độ dài tối đa 15 ký tự.
 */
#define NVS_NAMESPACE          "app_cfg"

#define NVS_KEY_VERSION        "cfg_ver"
#define NVS_KEY_SSID           "ssid"
#define NVS_KEY_PASSWORD       "pass"
#define NVS_KEY_PROVISIONED    "valid"
#define NVS_KEY_PACKET_RATE    "pkt_hz"
#define NVS_KEY_THRESHOLD      "thr_milli"

#define PACKET_RATE_MIN_HZ       1
#define PACKET_RATE_MAX_HZ       1000

#define MOTION_THRESHOLD_MIN     0.0f
#define MOTION_THRESHOLD_MAX     100000.0f
#define NVS_KEY_THRESHOLD      "thr_milli"


// Pending credentials phục vụ đổi wifi an toàn
#define NVS_KEY_PENDING_VALID      "pending"
#define NVS_KEY_PENDING_SSID       "new_ssid"
#define NVS_KEY_PENDING_PASSWORD   "new_pass"
/*
 * NVS không hỗ trợ float trực tiếp.
 * Threshold sẽ được nhân 1000 rồi lưu bằng int32_t.
 */
#define THRESHOLD_SCALE          1000.0f

esp_err_t config_store_init(void)
{
    esp_err_t err = nvs_flash_init();

    /*
     * Nếu NVS hết page trống hoặc khác phiên bản,
     * xóa partition NVS và khởi tạo lại.
     */
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) ||
        (err == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        ESP_LOGW(TAG, "NVS requires erase and reinitialization");

        err = nvs_flash_erase();

        if (err != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "Failed to erase NVS: %s",
                esp_err_to_name(err)
            );

            return err;
        }

        err = nvs_flash_init();
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize NVS: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(TAG, "NVS initialized");

    return ESP_OK;
}

void config_store_set_defaults(app_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    /*
     * Xóa toàn bộ struct để bảo đảm các chuỗi
     * có ký tự kết thúc '\0'.
     */
    memset(config, 0, sizeof(*config));

    config->version = APP_CONFIG_VERSION;
    config->packet_rate_hz = APP_DEFAULT_PACKET_RATE_HZ;
    config->motion_threshold = APP_DEFAULT_MOTION_THRESHOLD;
    config->provisioned = false;
}

bool config_store_validate(const app_config_t *config)
{
    size_t ssid_length;
    size_t password_length;

    if (config == NULL)
    {
        ESP_LOGE(TAG, "Config pointer is NULL");

        return false;
    }

    if (config->version != APP_CONFIG_VERSION)
    {
        ESP_LOGE(
            TAG,
            "Unsupported config version: %u",
            config->version
        );

        return false;
    }

    ssid_length = strnlen(
        config->ssid,
        sizeof(config->ssid)
    );

    /*
     * Nếu không có '\0' trong buffer, strnlen()
     * trả về kích thước toàn bộ buffer là 33.
     */
    if (ssid_length > APP_WIFI_SSID_MAX_LEN)
    {
        ESP_LOGE(TAG, "SSID is too long or not terminated");

        return false;
    }

    password_length = strnlen(
        config->password,
        sizeof(config->password)
    );

    if (password_length > APP_WIFI_PASSWORD_MAX_LEN)
    {
        ESP_LOGE(TAG, "Password is too long or not terminated");

        return false;
    }

    /*
     * Nếu thiết bị được đánh dấu provisioned,
     * SSID bắt buộc phải tồn tại.
     */
    if (config->provisioned && (ssid_length == 0))
    {
        ESP_LOGE(
            TAG,
            "Provisioned config must contain an SSID"
        );

        return false;
    }

    /*
     * Password rỗng được sử dụng cho mạng Wi-Fi open.
     * Nếu có password thì yêu cầu ít nhất 8 ký tự.
     */
    if ((password_length > 0) && (password_length < 8))
    {
        ESP_LOGE(
            TAG,
            "Password must be empty or at least 8 characters"
        );

        return false;
    }

    if ((config->packet_rate_hz < PACKET_RATE_MIN_HZ) ||
        (config->packet_rate_hz > PACKET_RATE_MAX_HZ))
    {
        ESP_LOGE(
            TAG,
            "Invalid packet rate: %u Hz",
            config->packet_rate_hz
        );

        return false;
    }

    if (!isfinite(config->motion_threshold))
    {
        ESP_LOGE(TAG, "Motion threshold is not finite");

        return false;
    }

    if ((config->motion_threshold < MOTION_THRESHOLD_MIN) ||
        (config->motion_threshold > MOTION_THRESHOLD_MAX))
    {
        ESP_LOGE(
            TAG,
            "Invalid motion threshold: %.3f",
            config->motion_threshold
        );

        return false;
    }

    return true;
}

esp_err_t config_store_save(const app_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err;

    int32_t threshold_milli;
    uint8_t provisioned;

    if (!config_store_validate(config))
    {
        return ESP_ERR_INVALID_ARG;
    }

    threshold_milli =
        (int32_t)((config->motion_threshold *
                   THRESHOLD_SCALE) + 0.5f);

    provisioned = config->provisioned ? 1U : 0U;

    err = nvs_open(
        NVS_NAMESPACE,
        NVS_READWRITE,
        &handle
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to open NVS: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err = nvs_set_u8(
        handle,
        NVS_KEY_VERSION,
        config->version
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_set_str(
        handle,
        NVS_KEY_SSID,
        config->ssid
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_set_str(
        handle,
        NVS_KEY_PASSWORD,
        config->password
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_set_u16(
        handle,
        NVS_KEY_PACKET_RATE,
        config->packet_rate_hz
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_set_i32(
        handle,
        NVS_KEY_THRESHOLD,
        threshold_milli
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    /*
     * Ghi cờ provisioned sau các trường dữ liệu.
     */
    err = nvs_set_u8(
        handle,
        NVS_KEY_PROVISIONED,
        provisioned
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    /*
     * Hoàn tất thao tác ghi vào flash.
     */
    err = nvs_commit(handle);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    ESP_LOGI(TAG, "Configuration saved");

cleanup:
    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to save configuration: %s",
            esp_err_to_name(err)
        );
    }

    nvs_close(handle);

    return err;
}

esp_err_t config_store_load(app_config_t *config)
{
    nvs_handle_t handle;
    app_config_t temporary_config;

    esp_err_t err;

    size_t ssid_size;
    size_t password_size;

    int32_t threshold_milli;
    uint8_t provisioned;

    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Chỉ sao chép temporary_config sang config
     * nếu toàn bộ dữ liệu được đọc thành công.
     */
    config_store_set_defaults(&temporary_config);

    err = nvs_open(
        NVS_NAMESPACE,
        NVS_READONLY,
        &handle
    );

    /*
     * Namespace chưa tồn tại là trạng thái bình thường
     * trong lần khởi động đầu tiên.
     */
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        config_store_set_defaults(config);

        ESP_LOGI(TAG, "No saved configuration");

        /*
         * Chuyển lỗi riêng của NVS thành lỗi tổng quát
         * cho tầng application.
         */
        return ESP_ERR_NOT_FOUND;
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to open NVS: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err = nvs_get_u8(
        handle,
        NVS_KEY_VERSION,
        &temporary_config.version
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    if (temporary_config.version != APP_CONFIG_VERSION)
    {
        ESP_LOGE(
            TAG,
            "Config version mismatch: stored=%u expected=%u",
            temporary_config.version,
            APP_CONFIG_VERSION
        );

        err = ESP_ERR_INVALID_VERSION;
        goto cleanup;
    }

    ssid_size = sizeof(temporary_config.ssid);

    err = nvs_get_str(
        handle,
        NVS_KEY_SSID,
        temporary_config.ssid,
        &ssid_size
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    password_size = sizeof(temporary_config.password);

    err = nvs_get_str(
        handle,
        NVS_KEY_PASSWORD,
        temporary_config.password,
        &password_size
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_get_u16(
        handle,
        NVS_KEY_PACKET_RATE,
        &temporary_config.packet_rate_hz
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_get_i32(
        handle,
        NVS_KEY_THRESHOLD,
        &threshold_milli
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    temporary_config.motion_threshold =
        threshold_milli / THRESHOLD_SCALE;

    err = nvs_get_u8(
        handle,
        NVS_KEY_PROVISIONED,
        &provisioned
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    temporary_config.provisioned =
        (provisioned != 0U);

    if (!config_store_validate(&temporary_config))
    {
        ESP_LOGE(TAG, "Stored configuration is invalid");

        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    /*
     * Chỉ cập nhật output khi toàn bộ cấu hình hợp lệ.
     */
    *config = temporary_config;

    ESP_LOGI(TAG, "Configuration loaded");

    err = ESP_OK;

cleanup:
    nvs_close(handle);

    /*
     * Namespace đã tồn tại nhưng bị thiếu key.
     */
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        config_store_set_defaults(config);

        ESP_LOGW(
            TAG,
            "Configuration is incomplete: a key is missing"
        );

        return ESP_ERR_NOT_FOUND;
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to load configuration: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    return ESP_OK;
}

esp_err_t config_store_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(
        NVS_NAMESPACE,
        NVS_READWRITE,
        &handle
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to open namespace for erase: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err = nvs_erase_all(handle);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to erase configuration: %s",
            esp_err_to_name(err)
        );

        nvs_close(handle);

        return err;
    }

    err = nvs_commit(handle);

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to commit erase: %s",
            esp_err_to_name(err)
        );

        nvs_close(handle);

        return err;
    }

    nvs_close(handle);

    ESP_LOGI(TAG, "Configuration erased");

    return ESP_OK;
}

bool config_store_has_credentials(const app_config_t *config)
{
    size_t ssid_length;

    if (config == NULL)
    {
        return false;
    }

    ssid_length = strnlen(
        config->ssid,
        sizeof(config->ssid)
    );

    return config->provisioned &&
           (ssid_length > 0) &&
           (ssid_length <= APP_WIFI_SSID_MAX_LEN);
}

esp_err_t config_store_save_pending(
    const app_config_t *config
)
{
    nvs_handle_t handle;
    esp_err_t err;

    if ((config == NULL) ||
        !config_store_validate(config) ||
        !config_store_has_credentials(config))
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(
        NVS_NAMESPACE,
        NVS_READWRITE,
        &handle
    );

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot open NVS for pending config: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    // Đánh dấu pending chưa hợp lệ
    err = nvs_set_u8(
        handle,
        NVS_KEY_PENDING_VALID,
        0
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_commit(handle);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    /*
     * Ghi credentials mới.
     */
    err = nvs_set_str(
        handle,
        NVS_KEY_PENDING_SSID,
        config->ssid
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_set_str(
        handle,
        NVS_KEY_PENDING_PASSWORD,
        config->password
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_commit(handle);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    /*
     * Chỉ đánh dấu hợp lệ sau khi hai chuỗi
     * đã được commit thành công.
     */
    err = nvs_set_u8(
        handle,
        NVS_KEY_PENDING_VALID,
        1
    );

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_commit(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Pending Wi-Fi configuration saved"
        );
    }

cleanup:
    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Cannot save pending config: %s",
            esp_err_to_name(err)
        );
    }

    nvs_close(handle);

    return err;
}

esp_err_t config_store_load_pending(
    app_config_t *config
)
{
    nvs_handle_t handle;
    esp_err_t err;

    uint8_t pending_valid = 0;

    char pending_ssid[
        APP_WIFI_SSID_MAX_LEN + 1
    ] = {0};

    char pending_password[
        APP_WIFI_PASSWORD_MAX_LEN + 1
    ] = {0};

    size_t ssid_size = sizeof(pending_ssid);
    size_t password_size = sizeof(pending_password);

    app_config_t temporary_config;

    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(
        NVS_NAMESPACE,
        NVS_READONLY,
        &handle
    );

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_get_u8(
        handle,
        NVS_KEY_PENDING_VALID,
        &pending_valid
    );

    if ((err == ESP_ERR_NVS_NOT_FOUND) ||
        ((err == ESP_OK) && (pending_valid == 0)))
    {
        nvs_close(handle);

        return ESP_ERR_NOT_FOUND;
    }

    if (err != ESP_OK)
    {
        nvs_close(handle);

        return err;
    }

    err = nvs_get_str(
        handle,
        NVS_KEY_PENDING_SSID,
        pending_ssid,
        &ssid_size
    );

    if (err != ESP_OK)
    {
        nvs_close(handle);

        return err == ESP_ERR_NVS_NOT_FOUND
            ? ESP_ERR_NOT_FOUND
            : err;
    }

    err = nvs_get_str(
        handle,
        NVS_KEY_PENDING_PASSWORD,
        pending_password,
        &password_size
    );

    nvs_close(handle);

    if (err != ESP_OK)
    {
        return err == ESP_ERR_NVS_NOT_FOUND
            ? ESP_ERR_NOT_FOUND
            : err;
    }

    /*
     * Nếu có active config thì giữ lại packet rate
     * và threshold. Nếu không thì dùng mặc định.
     */
    if (config_store_load(&temporary_config) != ESP_OK)
    {
        config_store_set_defaults(&temporary_config);
    }

    memset(
        temporary_config.ssid,
        0,
        sizeof(temporary_config.ssid)
    );

    memset(
        temporary_config.password,
        0,
        sizeof(temporary_config.password)
    );

    memcpy(
        temporary_config.ssid,
        pending_ssid,
        strnlen(
            pending_ssid,
            sizeof(pending_ssid)
        )
    );

    memcpy(
        temporary_config.password,
        pending_password,
        strnlen(
            pending_password,
            sizeof(pending_password)
        )
    );

    temporary_config.version = APP_CONFIG_VERSION;
    temporary_config.provisioned = true;

    if (!config_store_validate(&temporary_config))
    {
        ESP_LOGE(TAG, "Pending config is invalid");

        return ESP_ERR_INVALID_STATE;
    }

    *config = temporary_config;

    ESP_LOGI(TAG, "Pending Wi-Fi configuration loaded");

    return ESP_OK;
}

esp_err_t config_store_erase_pending(void)
{
    nvs_handle_t handle;
    esp_err_t err;
    esp_err_t erase_err;

    err = nvs_open(
        NVS_NAMESPACE,
        NVS_READWRITE,
        &handle
    );

    if (err != ESP_OK)
    {
        return err;
    }

    erase_err = nvs_erase_key(
        handle,
        NVS_KEY_PENDING_VALID
    );

    if ((erase_err != ESP_OK) &&
        (erase_err != ESP_ERR_NVS_NOT_FOUND))
    {
        nvs_close(handle);

        return erase_err;
    }

    erase_err = nvs_erase_key(
        handle,
        NVS_KEY_PENDING_SSID
    );

    if ((erase_err != ESP_OK) &&
        (erase_err != ESP_ERR_NVS_NOT_FOUND))
    {
        nvs_close(handle);

        return erase_err;
    }

    erase_err = nvs_erase_key(
        handle,
        NVS_KEY_PENDING_PASSWORD
    );

    if ((erase_err != ESP_OK) &&
        (erase_err != ESP_ERR_NVS_NOT_FOUND))
    {
        nvs_close(handle);

        return erase_err;
    }

    err = nvs_commit(handle);

    nvs_close(handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Pending configuration erased");
    }

    return err;
}

esp_err_t config_store_promote_pending(void)
{
    app_config_t pending_config;
    esp_err_t err;

    err = config_store_load_pending(
        &pending_config
    );

    if (err != ESP_OK)
    {
        return err;
    }

    /*
     * Ghi pending sang active config.
     */
    err = config_store_save(
        &pending_config
    );

    if (err != ESP_OK)
    {
        return err;
    }

    /*
     * Chỉ xóa pending sau khi active đã commit.
     */
    err = config_store_erase_pending();

    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(
        TAG,
        "Pending configuration promoted to active"
    );

    return ESP_OK;
}