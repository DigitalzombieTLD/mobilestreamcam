/*
 * config.h - esp32c3_wifi_stream configuration
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

/* ---- SPI link from the HX6538 (HX = master, ESP32-C3 = slave) ----
 * Grove Vision AI V2 XIAO header (verified against the Seeed schematic, see docs/mjpeg_stream.md):
 *   HX PB4  SCLK -> XIAO D8  = GPIO8
 *   HX PB2  MOSI -> XIAO D10 = GPIO10
 *   HX PB3  MISO <- XIAO D9  = GPIO9
 *   HX PB11 CS   -> XIAO D1  = GPIO3
 */
#ifndef SPI_PIN_SCLK
#define SPI_PIN_SCLK              8
#define SPI_PIN_MOSI              10
#define SPI_PIN_MISO              9
#define SPI_PIN_CS                3
#endif
#ifndef SPI_MODE_NUM
#define SPI_MODE_NUM              0          /* UNVERIFIED: SPI mode used by the HX6538 master */
#endif

/* ---- Himax SPI protocol framing (UNVERIFIED without hardware) ----
 * header: [0xC0][0x5A][type][len, 4 bytes little endian] followed by the payload.
 */
#define PTL_MAGIC0                0xC0
#define PTL_MAGIC1                0x5A
#define PTL_HEADER_LEN            7
#define PTL_TYPE_JPG              0x01

/* ---- Frame buffers (internal RAM, ~400 KB total on the C3) ---- */
#define FRAME_MAX_BYTES           (24 * 1024)  /* hard limit: larger frames are dropped (counted as bad). 640x480 is ~5.7 KB typical.
                                                  The HX6538 may send up to MJPEG_MAX_FRAME_BYTES (56 KB), the ESP32-C3 drops above this. */
#define FRAME_SLOT_COUNT          3            /* 1 receiving + 1 latest + 1 being sent; use 2 if RAM is short */

/* ---- HTTP ---- */
#define HTTP_PORT                 80
#define MAX_STREAM_CLIENTS        1            /* only one viewer is ever needed; a new /stream request replaces the old one */

/* ---- Health / low-heap watchdog ---- */
#define HEALTH_PERIOD_MS          1000
#define HEALTH_LOG_EVERY_S        10
#define LOW_HEAP_BYTES            (6 * 1024)   /* free heap or largest block below this is "low" */
#define LOW_HEAP_DROP_CLIENT_S    5            /* low for this long: drop the stream client */
#define LOW_HEAP_RESTART_S        10           /* still low after this long: esp_restart() */
