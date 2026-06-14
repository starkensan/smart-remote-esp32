#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"
#endif

#ifndef D1
#define D1 4
#endif

#ifndef D2
#define D2 15
#endif

#ifndef D3
#define D3 16
#endif

#define IR_SEND_PIN D1
#define IR_RECV_PIN D2
#define CONFIG_BUTTON_PIN D3
#define STATUS_LED_PIN -1

#define HTTP_PORT 80
#define DEVICE_NAME "esp32-smart-remote"
