#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*wifi_got_ip_cb_t)(void);

typedef struct {
    bool connected;
    bool associated;
    bool ap_info_valid;
    char ssid[34];
    char bssid[18];
    char phy[16];
    char secondary_offset[8];
    char power_save[12];
    int rssi;
    int primary_channel;
    int secondary_channel;
    int bandwidth_mhz;
    bool ap_11b;
    bool ap_11g;
    bool ap_11n;
    uint32_t associations;
    uint32_t disconnects;
    int last_disconnect_reason;
} wifi_sta_diagnostics_t;

void wifi_sta_start(wifi_got_ip_cb_t on_got_ip);
bool wifi_sta_connected(void);
int  wifi_sta_rssi(void);              /* dBm, 0 if not connected */
void wifi_sta_ip_str(char *buf, int len);
void wifi_sta_get_diagnostics(wifi_sta_diagnostics_t *diagnostics);
