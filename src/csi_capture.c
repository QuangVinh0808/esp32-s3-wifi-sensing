#include "csi_capture.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "CSI_CAPTURE";

#define CSI_RAW_QUEUE_LENGTH    32U

static QueueHandle_t s_raw_queue = NULL;
static uint8_t s_router_bssid[6] = {0};
static uint8_t s_router_channel = 0U;
static uint32_t s_last_timestamp_us = 0U;
static bool s_timestamp_initialized = false;
static volatile bool s_running = false;

static uint32_t s_received_packets = 0U;
static uint32_t s_dropped_packets = 0U;
static uint32_t s_invalid_packets = 0U;

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

static void stats_increment(uint32_t *counter)
{
    portENTER_CRITICAL(&s_stats_lock);
    (*counter)++;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void reset_statistics(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_received_packets = 0U;
    s_dropped_packets = 0U;
    s_invalid_packets = 0U;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void csi_receive_callback(void *context, wifi_csi_info_t *info)
{
    csi_raw_sample_t sample = {0};
    (void)context;

    if (!s_running)
    {
        return;
    }

    if ((info == NULL) || (info->buf == NULL))
    {
        stats_increment(&s_invalid_packets);
        return;
    }

    if (memcmp(info->mac, s_router_bssid, sizeof(s_router_bssid)) != 0)
    {
        return;
    }

    if ((info->len < CSI_LLTF_BYTE_COUNT) ||
        info->first_word_invalid ||
        (info->rx_ctrl.channel != s_router_channel))
    {
        stats_increment(&s_invalid_packets);
        return;
    }

    if (s_timestamp_initialized &&
        ((int32_t)(info->rx_ctrl.timestamp - s_last_timestamp_us) <= 0))
    {
        stats_increment(&s_invalid_packets);
        return;
    }

    s_last_timestamp_us = info->rx_ctrl.timestamp;
    s_timestamp_initialized = true;

    sample.timestamp_us = info->rx_ctrl.timestamp;
    sample.rssi = info->rx_ctrl.rssi;
    sample.noise_floor = info->rx_ctrl.noise_floor;
    sample.channel = info->rx_ctrl.channel;
    sample.first_word_invalid = false;

    memcpy(sample.source_mac, info->mac, sizeof(sample.source_mac));

    sample.csi_len = CSI_LLTF_BYTE_COUNT;
    memcpy(sample.data, info->buf, CSI_LLTF_BYTE_COUNT);

    stats_increment(&s_received_packets);

    if (xQueueSend(s_raw_queue, &sample, 0U) != pdTRUE)
    {
        stats_increment(&s_dropped_packets);
    }
}

esp_err_t csi_capture_start(void)
{
    wifi_ap_record_t ap_info = {0};
    wifi_csi_config_t csi_config =
    {
        .lltf_en = true,
        .htltf_en = false,
        .stbc_htltf2_en = false,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = true,
        .shift = true
    };
    esp_err_t err;

    if (s_running)
    {
        return ESP_OK;
    }

    if (!wifi_manager_is_connected())
    {
        return ESP_ERR_INVALID_STATE;
    }

    err = wifi_manager_get_ap_info(&ap_info);
    if (err != ESP_OK)
    {
        return err;
    }

    memcpy(s_router_bssid, ap_info.bssid, sizeof(s_router_bssid));
    s_router_channel = ap_info.primary;
    s_last_timestamp_us = 0U;
    s_timestamp_initialized = false;

    if (s_raw_queue == NULL)
    {
        s_raw_queue = xQueueCreate(CSI_RAW_QUEUE_LENGTH, sizeof(csi_raw_sample_t));
        if (s_raw_queue == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    else
    {
        (void)xQueueReset(s_raw_queue);
    }

    reset_statistics();

    err = esp_wifi_set_csi_config(&csi_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "CSI config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_csi_rx_cb(csi_receive_callback, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "CSI callback registration failed: %s", esp_err_to_name(err));
        return err;
    }

    s_running = true;
    err = esp_wifi_set_csi(true);
    if (err != ESP_OK)
    {
        s_running = false;
        ESP_LOGE(TAG, "CSI enable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "CSI capture started");
    ESP_LOGI(TAG, "Router BSSID: " MACSTR, MAC2STR(s_router_bssid));
    ESP_LOGI(TAG, "Router channel: %u", (unsigned int)ap_info.primary);

    return ESP_OK;
}

esp_err_t csi_capture_stop(void)
{
    esp_err_t err;

    if (!s_running)
    {
        return ESP_OK;
    }

    s_running = false;
    err = esp_wifi_set_csi(false);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "CSI disable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "CSI capture stopped");
    return ESP_OK;
}

bool csi_capture_is_running(void)
{
    return s_running;
}

QueueHandle_t csi_capture_get_queue(void)
{
    return s_raw_queue;
}

void csi_capture_get_stats(csi_capture_stats_t *stats)
{
    if (stats == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&s_stats_lock);
    stats->received_packets = s_received_packets;
    stats->dropped_packets = s_dropped_packets;
    stats->invalid_packets = s_invalid_packets;
    portEXIT_CRITICAL(&s_stats_lock);
}
