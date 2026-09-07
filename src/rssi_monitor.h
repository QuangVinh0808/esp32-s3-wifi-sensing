#ifndef RSSI_MONITOR_H
#define RSSI_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int64_t timestamp_ms;
    uint32_t sequence;
    int8_t rssi;
    uint8_t channel;
} rssi_sample_t;

esp_err_t rssi_monitor_start(uint16_t sample_rate_hz);
esp_err_t rssi_monitor_stop(void);
bool rssi_monitor_is_running(void);
QueueHandle_t rssi_monitor_get_queue(void);

#ifdef __cplusplus
}
#endif

#endif