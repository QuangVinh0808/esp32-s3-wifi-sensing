#ifndef CSI_PROCESSOR_H
#define CSI_PROCESSOR_H

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t csi_processor_start(QueueHandle_t raw_queue);
esp_err_t csi_processor_stop(void);
bool csi_processor_is_running(void);
QueueHandle_t csi_processor_get_queue(void);

#ifdef __cplusplus
}
#endif

#endif
