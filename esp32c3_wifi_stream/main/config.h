/*
 * config.h - esp32c3_wifi_stream configuration (AI-Thinker ESP32-CAM)
 *
 * Do NOT commit real credentials. Put your overrides into main/config_local.h
 * (git-ignored), e.g.:
 *     #define WIFI_SSID     "MyNetwork"
 *     #define WIFI_PASSWORD "MyPassword"
 */
#pragma once

#if __has_include("config_local.h")
#include "config_local.h"
#endif

/* ---- WiFi ---- */
#ifndef WIFI_SSID
#define WIFI_SSID                 "SteamCam"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD             "StreamCam123"   /* "" for an open network */
#endif
#ifndef DEVICE_HOSTNAME
#define DEVICE_HOSTNAME           "mobilestreamcam"       /* also advertised as <hostname>.local (mDNS) */
#endif

/* Optional static IP (0 = DHCP) */
#ifndef USE_STATIC_IP
#define USE_STATIC_IP             1
#endif
#ifndef STATIC_IP
#define STATIC_IP                 "10.153.239.222"
#define STATIC_GATEWAY            "10.153.239.1"
#define STATIC_NETMASK            "255.255.255.0"
#define STATIC_DNS                "10.153.239.1"
#endif

/* ---- Camera: AI-Thinker ESP32-CAM, OV2640 ----
 * Default pin map of the common AI-Thinker board (also used by the ESP32-CAM-MB programming adapter setup).
 * Other ESP32-CAM variants (M5Stack, TTGO, Wrover-Kit, ...) use a different map: override in main/config_local.h.
 * Note: GPIO4 = flash LED, GPIO12-15 = SD card (unused), GPIO0 = XCLK (also the boot strapping pin).
 */
#ifndef CAM_PIN_PWDN
#define CAM_PIN_PWDN              32
#define CAM_PIN_RESET             (-1)
#define CAM_PIN_XCLK              0
#define CAM_PIN_SIOD              26
#define CAM_PIN_SIOC              27
#define CAM_PIN_D7                35
#define CAM_PIN_D6                34
#define CAM_PIN_D5                39
#define CAM_PIN_D4                36
#define CAM_PIN_D3                21
#define CAM_PIN_D2                19
#define CAM_PIN_D1                18
#define CAM_PIN_D0                5
#define CAM_PIN_VSYNC             25
#define CAM_PIN_HREF              23
#define CAM_PIN_PCLK              22
#endif
#ifndef CAM_XCLK_FREQ_HZ
#define CAM_XCLK_FREQ_HZ          20000000
#endif
#ifndef CAM_FRAME_SIZE
#define CAM_FRAME_SIZE            FRAMESIZE_VGA   /* 640x480 */
#endif
#ifndef CAM_JPEG_QUALITY
#define CAM_JPEG_QUALITY          12              /* 0-63, lower = better quality / bigger frames */
#endif
#ifndef CAM_FB_COUNT
#define CAM_FB_COUNT              2               /* driver frame buffers (PSRAM) */
#endif

/* ---- Frame slots (PSRAM). The camera task copies each JPEG out of the driver's frame buffer into a slot and
 * returns the buffer immediately, so a slow HTTP client never blocks the camera driver. ---- */
#define FRAME_MAX_BYTES           (96 * 1024)  /* larger frames are dropped (counted as bad). VGA JPEG is typically 15-40 KB */
#define FRAME_SLOT_COUNT          3            /* 1 being written + 1 latest + 1 being sent */

/* ---- HTTP ---- */
#define HTTP_PORT                 80
#define MAX_STREAM_CLIENTS        1            /* only one viewer is ever needed; a new /stream request replaces the old one */

/* ---- Health / low-heap watchdog ---- */
#define HEALTH_PERIOD_MS          1000
#define HEALTH_LOG_EVERY_S        10
#define LOW_HEAP_BYTES            (6 * 1024)   /* free heap or largest block below this is "low" */
#define LOW_HEAP_DROP_CLIENT_S    5            /* low for this long: drop the stream client */
#define LOW_HEAP_RESTART_S        10           /* still low after this long: esp_restart() */
