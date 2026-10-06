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
#define WIFI_SSID                 "SchachtCam"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD             "SchachtCam123!"   /* "" for an open network */
#endif
#ifndef DEVICE_HOSTNAME
#define DEVICE_HOSTNAME           "SchachtCam"       /* also advertised as <hostname>.local (mDNS) */
#endif

/* Optional static IP (0 = DHCP) */
#ifndef USE_STATIC_IP
#define USE_STATIC_IP             0
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
/* Quality / speed presets (select with "#define CAM_PRESET n" in main/config_local.h). Frame rates are ESTIMATES for
 * the OV2640 at 20 MHz XCLK over Wi-Fi and have NOT been measured on hardware: check /status "fps" and tune.
 *   1 = SVGA 800x600,   quality 10  (default; expected to reach the 10-15+ fps goal)
 *   2 = VGA  640x480,   quality 12  (previous default; fastest)
 *   3 = XGA  1024x768,  quality 12  (sharper; fps depends heavily on Wi-Fi, may drop below 10)
 *   4 = SXGA 1280x1024, quality 14  (slow, probably < 8 fps)
 *   5 = UXGA 1600x1200, quality 16  (slowest, a few fps)
 * CAM_FRAME_SIZE / CAM_JPEG_QUALITY / FRAME_MAX_BYTES can still be overridden individually. */
#ifndef CAM_PRESET
#define CAM_PRESET                2
#endif
#if CAM_PRESET == 2
#define CAM_PRESET_FRAME_SIZE     FRAMESIZE_VGA
#define CAM_PRESET_QUALITY        12
#define CAM_PRESET_MAX_BYTES      (96 * 1024)
#elif CAM_PRESET == 3
#define CAM_PRESET_FRAME_SIZE     FRAMESIZE_XGA
#define CAM_PRESET_QUALITY        12
#define CAM_PRESET_MAX_BYTES      (160 * 1024)
#elif CAM_PRESET == 4
#define CAM_PRESET_FRAME_SIZE     FRAMESIZE_SXGA
#define CAM_PRESET_QUALITY        14
#define CAM_PRESET_MAX_BYTES      (224 * 1024)
#elif CAM_PRESET == 5
#define CAM_PRESET_FRAME_SIZE     FRAMESIZE_UXGA
#define CAM_PRESET_QUALITY        16
#define CAM_PRESET_MAX_BYTES      (320 * 1024)
#else
#define CAM_PRESET_FRAME_SIZE     FRAMESIZE_SVGA
#define CAM_PRESET_QUALITY        10
#define CAM_PRESET_MAX_BYTES      (128 * 1024)
#endif

#ifndef CAM_FRAME_SIZE
#define CAM_FRAME_SIZE            CAM_PRESET_FRAME_SIZE
#endif
#ifndef CAM_JPEG_QUALITY
#define CAM_JPEG_QUALITY          CAM_PRESET_QUALITY   /* 0-63, lower = better quality / bigger frames */
#endif
#ifndef CAM_FB_COUNT
#define CAM_FB_COUNT              2               /* driver frame buffers (PSRAM); 2 = capture overlaps the copy */
#endif

/* ---- Frame slots (PSRAM). The camera task copies each JPEG out of the driver's frame buffer into a slot and
 * returns the buffer immediately, so a slow HTTP client never blocks the camera driver. ---- */
#ifndef FRAME_MAX_BYTES
#define FRAME_MAX_BYTES           CAM_PRESET_MAX_BYTES  /* larger frames are dropped (counted as bad), never written past the slot */
#endif
#ifndef FRAME_SLOT_COUNT
#define FRAME_SLOT_COUNT          3            /* 1 being written + 1 latest + 1 being sent */
#endif
/* PSRAM budget: the 4 MB usable PSRAM must hold the slots plus the driver frame buffers (CAM_FB_COUNT x ~FRAME_MAX_BYTES).
 * Keep slots + driver buffers well under 3 MB so nothing starves. */
_Static_assert((FRAME_SLOT_COUNT + CAM_FB_COUNT) * FRAME_MAX_BYTES <= 3 * 1024 * 1024,
               "frame store + camera buffers exceed the PSRAM budget: lower FRAME_MAX_BYTES / FRAME_SLOT_COUNT / CAM_FB_COUNT");

/* ---- HTTP ---- */
#define HTTP_PORT                 80
#define MAX_STREAM_CLIENTS        1            /* only one viewer is ever needed; a new /stream request replaces the old one */

/* ---- Health / low-heap watchdog ---- */
#define HEALTH_PERIOD_MS          1000
#define HEALTH_LOG_EVERY_S        10
#define LOW_HEAP_BYTES            (6 * 1024)   /* free heap or largest block below this is "low" */
#define LOW_HEAP_DROP_CLIENT_S    5            /* low for this long: drop the stream client */
#define LOW_HEAP_RESTART_S        10           /* still low after this long: esp_restart() */
