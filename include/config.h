#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"
#endif

#define IR_SEND_PIN 4
#define IR_RECV_PIN 15
#define STATUS_LED_PIN 2

#define HTTP_PORT 80
#define DEVICE_NAME "esp32-smart-remote"
