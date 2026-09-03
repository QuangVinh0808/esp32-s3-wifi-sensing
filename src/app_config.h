#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define APP_WIFI_SSID_MAX_LEN 32
#define APP_WIFI_PASSWORD_MAX_LEN 64

#define APP_CONFIG_VERSION 1

#define APP_DEFAULT_PACKET_RATE_HZ 20
#define APP_DEFAULT_MOTION_THRESHOLD 3.0f

typedef struct {
    uint8_t version;
    char ssid[APP_WIFI_SSID_MAX_LEN + 1]; //Luu them ki tu ket thuc '\0' cho chuoi
    char password[APP_WIFI_PASSWORD_MAX_LEN + 1];
    uint16_t packet_rate_hz;
    float motion_threshold;
    bool provisioned;
} app_config_t;

#endif // APP_CONFIG_H