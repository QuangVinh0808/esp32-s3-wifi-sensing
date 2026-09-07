#ifndef CSI_TRAFFIC_H
#define CSI_TRAFFIC_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t csi_traffic_start(uint16_t packet_rate_hz);
esp_err_t csi_traffic_stop(void);
bool csi_traffic_is_running(void);

#ifdef __cplusplus
}
#endif

#endif
