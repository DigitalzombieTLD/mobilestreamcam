/*
 * http_stream.c - /, /stream (MJPEG), /snapshot.jpg, /status
 *
 * /stream runs in its own task per client (async handler) so the HTTP server thread stays
 * responsive. A client always gets the newest frame; frames it is too slow for are skipped.
 */
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "frame_store.h"
#include "wifi_sta.h"
#include "http_stream.h"

static const char *TAG = "http";

#define BOUNDARY "mobilestreamcamframe"

static httpd_handle_t s_server;
static atomic_int s_clients;

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>mobilestreamcam</title>"
    "<style>body{margin:0;background:#111;color:#ccc;font-family:sans-serif;text-align:center}"
    "img{max-width:100%;height:auto}a{color:#8cf}</style></head><body>"
    "<img src=\"/stream\" alt=\"stream\">"
    "<p><a href=\"/snapshot.jpg\">snapshot</a> | <a href=\"/status\">status</a></p>"
    "</body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t snapshot_handler(httpd_req_t *req)
{
    frame_slot_t *f = frame_store_acquire_latest(0, 3000);
    if (!f) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "no frame available\n", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=snapshot.jpg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, (const char *)f->data + f->offset, f->size);
    frame_store_release(f);
    return r;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    frame_stats_t st;
    char ip[16];
    char buf[400];
    frame_store_get_stats(&st);
    wifi_sta_ip_str(ip, sizeof(ip));
    int n = snprintf(buf, sizeof(buf),
        "{\"fps\":%.1f,\"width\":%u,\"height\":%u,\"last_frame_bytes\":%u,"
        "\"frames\":%u,\"bad_frames\":%u,\"dropped_frames\":%u,\"clients\":%d,"
        "\"wifi\":{\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\"},"
        "\"heap_free\":%u,\"heap_min_free\":%u,\"uptime_s\":%u}",
        st.fps, st.width, st.height, (unsigned)st.last_size,
        (unsigned)st.frames, (unsigned)st.bad, (unsigned)st.dropped, (int)atomic_load(&s_clients),
        WIFI_SSID, wifi_sta_rssi(), ip,
        (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)(esp_timer_get_time() / 1000000));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

static void stream_task(void *arg)
{
    httpd_req_t *req = arg;
    uint32_t last_seq = 0;
    char hdr[160];

    if (httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" BOUNDARY) != ESP_OK) {
        goto out;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    for (;;) {
        frame_slot_t *f = frame_store_acquire_latest(last_seq, 5000);
        if (!f) {
            /* no new frame yet (HX restarting?): a keepalive detects clients that went away */
            if (httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
                break;
            }
            continue;
        }
        last_seq = f->seq;
        int hl = snprintf(hdr, sizeof(hdr),
                          "--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                          (unsigned)f->size);
        esp_err_t r = httpd_resp_send_chunk(req, hdr, hl);
        if (r == ESP_OK) {
            r = httpd_resp_send_chunk(req, (const char *)f->data + f->offset, f->size);
        }
        if (r == ESP_OK) {
            r = httpd_resp_send_chunk(req, "\r\n", 2);
        }
        frame_store_release(f);
        if (r != ESP_OK) {
            break; /* client went away or too slow (send timeout) */
        }
    }
out:
    ESP_LOGI(TAG, "stream client left (%d active)", atomic_fetch_sub(&s_clients, 1) - 1);
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (atomic_load(&s_clients) >= MAX_STREAM_CLIENTS) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "too many stream clients\n", HTTPD_RESP_USE_STRLEN);
    }
    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        return ESP_FAIL;
    }
    atomic_fetch_add(&s_clients, 1);
    if (xTaskCreate(stream_task, "stream", 4096, copy, 5, NULL) != pdPASS) {
        atomic_fetch_sub(&s_clients, 1);
        httpd_req_async_handler_complete(copy);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "stream client joined (%d active)", (int)atomic_load(&s_clients));
    return ESP_OK;
}

void http_stream_start(void)
{
    if (s_server) {
        return;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.max_open_sockets = MAX_STREAM_CLIENTS + 3;
    cfg.max_uri_handlers = 6;
    cfg.lru_purge_enable = true;
    cfg.send_wait_timeout = 3;   /* drop clients that stall */
    cfg.recv_wait_timeout = 3;
    cfg.stack_size = 6144;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET, .handler = index_handler },
        { .uri = "/stream",       .method = HTTP_GET, .handler = stream_handler },
        { .uri = "/snapshot.jpg", .method = HTTP_GET, .handler = snapshot_handler },
        { .uri = "/status",       .method = HTTP_GET, .handler = status_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "HTTP server on port %d", HTTP_PORT);
}
