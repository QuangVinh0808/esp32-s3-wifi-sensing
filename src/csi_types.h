#ifndef CSI_TYPES_H
#define CSI_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#define CSI_LLTF_COMPLEX_COUNT    64U
#define CSI_LLTF_BYTE_COUNT       128U

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

    uint32_t received_packets;
    uint32_t dropped_packets;
} csi_processed_sample_t;

typedef struct
{
    uint32_t received_packets;
    uint32_t dropped_packets;
    uint32_t invalid_packets;
} csi_capture_stats_t;

#endif