#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

typedef enum {
    WIFI_MANAGER_STATE_UNINITIALIZED = 0,
    WIFI_MANAGER_STATE_INITIALIZED,
    WIFI_MANAGER_STATE_CONNECTING,
    WIFI_MANAGER_STATE_CONNECTED,
    WIFI_MANAGER_STATE_DISCONNECTED,
    WIFI_MANAGER_STATE_FAILED,

} wifi_manager_state_t;

esp_err_t wifi_manager_init(void);

esp_err_t wifi_manager_start(
    const app_config_t *config
);

esp_err_t wifi_manager_wait_connected(
    TickType_t timeout
);

wifi_manager_state_t wifi_manager_get_state(void);

bool wifi_manager_is_connected(void);

esp_err_t wifi_manager_get_ap_info(
    wifi_ap_record_t *ap_info
);

esp_err_t wifi_manager_stop(void);

#endif /* WIFI_MANAGER_H */