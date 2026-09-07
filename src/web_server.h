#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_server_start(QueueHandle_t csi_sample_queue);
esp_err_t web_server_stop(void);
bool web_server_is_running(void);

#ifdef __cplusplus
}
#endif

#endif
