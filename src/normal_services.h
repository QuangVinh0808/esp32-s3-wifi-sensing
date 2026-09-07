#ifndef NORMAL_SERVICES_H
#define NORMAL_SERVICES_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t normal_services_start(uint16_t csi_packet_rate_hz);
esp_err_t normal_services_stop(void);
bool normal_services_is_running(void);

#ifdef __cplusplus
}
#endif

#endif