#ifndef MOTION_DETECTOR_H
#define MOTION_DETECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "csi_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    float fast_change;
    float window_activity;
    float baseline_distance;
    float raw_score;
    float score;
    float threshold_low;
    float threshold_high;
    uint8_t calibration_progress;
    csi_motion_state_t state;
    bool calibrated;
} motion_result_t;

esp_err_t motion_detector_configure(uint16_t sample_rate_hz);
void motion_detector_request_calibration(void);
void motion_detector_process(
    const float *normalized_amplitudes,
    size_t subcarrier_count,
    motion_result_t *result
);
const char *motion_detector_state_name(csi_motion_state_t state);

#ifdef __cplusplus
}
#endif

#endif
