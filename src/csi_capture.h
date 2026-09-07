#ifndef CSI_CAPTURE_H
#define CSI_CAPTURE_H

#include <stdbool.h>

#include "csi_types.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t csi_capture_start(void);
esp_err_t csi_capture_stop(void);

bool csi_capture_is_running(void);

QueueHandle_t csi_capture_get_queue(void);

void csi_capture_get_stats(
    csi_capture_stats_t *stats
);

#ifdef __cplusplus
}
#endif

#endif