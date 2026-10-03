/*
 * wifi_sta.c - STA mode, scan + connect to the configured SSID, reconnect forever,
 * power save off, max TX power, mDNS.
 */
#include <stdio.h>
#include "sdkconfig.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "config.h"
#include "wifi_sta.h"

static const char *TAG = "wifi_sta";

static wifi_got_ip_cb_t s_on_got_ip;
static volatile bool s_connected;
static esp_netif_t *s_netif;
static uint32_t s_retry;
static esp_timer_handle_t s_retry_timer;

static void do_connect(void)
{
    esp_err_t r = esp_wifi_connect();
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(r));
    }
}

static void retry_cb(void *arg)
{
    if (!s_connected) {
        do_connect();
    }
}

/* Radio settings. Must NOT be changed while associating/connected: calling esp_wifi_set_bandwidth()
 * from the STA_CONNECTED handler tore the fresh connection down again ("disconnected (reason 8)"). */
static void tune_radio(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(84)); /* units of 0.25 dBm, clamped to the regulatory max */
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
}

static void start_mdns(void)
{
#if CONFIG_MSC_ENABLE_MDNS
    static bool started;
    if (started) {
        return;
    }
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(DEVICE_HOSTNAME);
        mdns_instance_name_set("mobilestreamcam");
        mdns_service_add(NULL, "_http", "_tcp", HTTP_PORT, NULL, 0);
        started = true;
        ESP_LOGI(TAG, "mDNS: %s.local", DEVICE_HOSTNAME);
    }
#endif
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "scanning for \"%s\"", WIFI_SSID);
            tune_radio();
            do_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            esp_timer_stop(s_retry_timer);
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = data;
            s_connected = false;
            s_retry++;
            ESP_LOGW(TAG, "disconnected (reason %d), retry #%u", d->reason, (unsigned)s_retry);
            /* retry forever; back off a little while the AP is not found (full scan each time) */
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, (s_retry < 5 ? 200 : 2000) * 1000ULL);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_retry = 0;
        esp_timer_stop(s_retry_timer);
        start_mdns();
        if (s_on_got_ip) {
            s_on_got_ip();
        }
    }
}

void wifi_sta_start(wifi_got_ip_cb_t on_got_ip)
{
    s_on_got_ip = on_got_ip;
    const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, DEVICE_HOSTNAME);

#if USE_STATIC_IP
    esp_netif_dhcpc_stop(s_netif);
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = esp_ip4addr_aton(STATIC_IP);
    ip.gw.addr = esp_ip4addr_aton(STATIC_GATEWAY);
    ip.netmask.addr = esp_ip4addr_aton(STATIC_NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &ip));
    esp_netif_dns_info_t dns = {0};
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(STATIC_DNS);
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); /* no NVS-backed WiFi config */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password));
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;           /* scan all channels for the SSID */
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;       /* best RSSI if several APs share the SSID */
    wc.sta.threshold.authmode = strlen(WIFI_PASSWORD) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;                        /* needed for WPA3-SAE; also fine for WPA2 */
    wc.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool wifi_sta_connected(void)
{
    return s_connected;
}

int wifi_sta_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

void wifi_sta_ip_str(char *buf, int len)
{
    esp_netif_ip_info_t ip;
    if (s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(buf, len, "0.0.0.0");
    }
}
