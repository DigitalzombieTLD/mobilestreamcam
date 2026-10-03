#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef void (*wifi_got_ip_cb_t)(void);

void wifi_sta_start(wifi_got_ip_cb_t on_got_ip);
bool wifi_sta_connected(void);
int  wifi_sta_rssi(void);              /* dBm, 0 if not connected */
void wifi_sta_ip_str(char *buf, int len);
