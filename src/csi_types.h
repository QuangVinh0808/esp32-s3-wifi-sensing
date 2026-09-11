#ifndef CSI_TYPES_H
#define CSI_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#define CSI_LLTF_COMPLEX_COUNT    64U
#define CSI_LLTF_BYTE_COUNT       128U
#define CSI_VALID_SUBCARRIER_COUNT 52U

typedef enum
{
    CSI_MOTION_STATE_WARMUP = 0,
    CSI_MOTION_STATE_CALIBRATING,
    CSI_MOTION_STATE_STATIC,
    CSI_MOTION_STATE_MOVING
} csi_motion_state_t;

typedef struct
{
    uint32_t timestamp_us;
    int8_t rssi;
    int8_t noise_floor;
    uint8_t channel;
    uint8_t source_mac[6];
    uint16_t csi_len;
    bool first_word_invalid;
    int8_t data[CSI_LLTF_BYTE_COUNT];
} csi_raw_sample_t;

typedef struct
{
    int64_t timestamp_ms;
    uint32_t sequence;
    int8_t rssi;
    int8_t noise_floor;
    uint8_t channel;
    uint16_t valid_subcarriers;
    float mean_power;
    float min_power;
    float max_power;
    float fast_change; // D[n]
    float window_activity; // V[n]
    float baseline_distance; // B[n] 
    float raw_motion_score;
    float motion_score;
    float motion_threshold_low;
    float motion_threshold_high;
    uint8_t calibration_progress;
    csi_motion_state_t motion_state;
    bool motion_calibrated;
    uint32_t received_packets;
    uint32_t dropped_packets;
    uint32_t invalid_packets;
} csi_processed_sample_t;

typedef struct
{
    uint32_t received_packets;
    uint32_t dropped_packets;
    uint32_t invalid_packets;
} csi_capture_stats_t;

#endif
