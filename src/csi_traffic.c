#include "csi_traffic.h"

#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "ping/ping_sock.h"

#include "lwip/ip_addr.h"

static const char *TAG = "CSI_TRAFFIC";

#define CSI_TRAFFIC_MIN_RATE_HZ    1U
#define CSI_TRAFFIC_MAX_RATE_HZ    100U
#define CSI_PING_STACK_SIZE        3072U

static esp_ping_handle_t s_ping_handle = NULL;
static bool s_running = false;

esp_err_t csi_traffic_start(uint16_t packet_rate_hz)
{
    esp_netif_t *station_netif;
    esp_netif_ip_info_t ip_info = {0};
    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    esp_ping_callbacks_t callbacks = {0};
    esp_err_t err;

    if (s_running)
    {
        return ESP_OK;
    }

    if (!wifi_manager_is_connected())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if ((packet_rate_hz < CSI_TRAFFIC_MIN_RATE_HZ) ||
        (packet_rate_hz > CSI_TRAFFIC_MAX_RATE_HZ))
    {
        return ESP_ERR_INVALID_ARG;
    }

    station_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (station_netif == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_netif_get_ip_info(station_netif, &ip_info);
    if (err != ESP_OK)
    {
        return err;
    }

    if (ip_info.gw.addr == 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ping_config.count = 0U;
    ping_config.interval_ms = 1000U / packet_rate_hz;
    ping_config.data_size = 1U;
    ping_config.task_stack_size = CSI_PING_STACK_SIZE;
    ping_config.target_addr.type = ESP_IPADDR_TYPE_V4;
    ping_config.target_addr.u_addr.ip4.addr = ip4_addr_get_u32(&ip_info.gw);

    err = esp_ping_new_session(&ping_config, &callbacks, &s_ping_handle);
    if (err != ESP_OK)
    {
        s_ping_handle = NULL;
        return err;
    }

    err = esp_ping_start(s_ping_handle);
    if (err != ESP_OK)
    {
        (void)esp_ping_delete_session(s_ping_handle);
        s_ping_handle = NULL;
        return err;
    }

    s_running = true;

    ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&ip_info.gw));
    ESP_LOGI(TAG, "Ping traffic started at %u Hz", (unsigned int)packet_rate_hz);

    return ESP_OK;
}

esp_err_t csi_traffic_stop(void)
{
    esp_err_t stop_err;
    esp_err_t delete_err;

    if (!s_running)
    {
        return ESP_OK;
    }

    stop_err = esp_ping_stop(s_ping_handle);
    delete_err = esp_ping_delete_session(s_ping_handle);

    s_ping_handle = NULL;
    s_running = false;

    if (stop_err != ESP_OK)
    {
        return stop_err;
    }

    if (delete_err != ESP_OK)
    {
        return delete_err;
    }

    ESP_LOGI(TAG, "Ping traffic stopped");
    return ESP_OK;
}

bool csi_traffic_is_running(void)
{
    return s_running;
}
