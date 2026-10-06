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
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "frame_store.h"
#include "camera.h"
#include "wifi_sta.h"
#include "http_stream.h"

static const char *TAG = "http";

#define BOUNDARY "mobilestreamcamframe"

static httpd_handle_t s_server;
static atomic_int s_clients;
static atomic_bool s_stop_stream;   /* ask the running stream task to leave */
static portMUX_TYPE s_tx_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_tx_frames;
static uint64_t s_tx_jpeg_bytes;

#define STREAM_TASK_STACK 3584

static const char INDEX_HEAD[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>mobilestreamcam</title>"
    "<style>body{margin:0;background:#111;color:#ccc;font-family:sans-serif;text-align:center}"
    "img{max-width:100%;height:auto}a{color:#8cf}#cam{margin:8px}small{display:block;max-width:34em;margin:4px auto;color:#999}"
    "input[type=number]{width:4em}#msg.err{color:#f88}</style></head><body>"
    "<img src=\"/stream\" alt=\"stream\">"
    "<div id=\"cam\">Current: <b id=\"cur\">";
static const char INDEX_MID[] =
    "</b><br>Resolution <select id=\"res\">";
static const char INDEX_QUALITY[] =
    "</select> JPEG quality <input id=\"q\" type=\"number\" min=\"" CAM_STR(CAMERA_QUALITY_MIN) "\" max=\""
    CAM_STR(CAMERA_QUALITY_MAX) "\" step=\"1\" value=\"";
static const char INDEX_TAIL[] =
    "\"> <button id=\"btn\" type=\"button\">Apply</button>"
    "<div id=\"msg\"></div>"
    "<small>JPEG quality " CAM_STR(CAMERA_QUALITY_MIN) "-" CAM_STR(CAMERA_QUALITY_MAX) ": a lower number = higher image quality "
    "and larger frames. Larger resolutions and lower quality numbers lower the frame rate.</small>"
    "<small id=\"per\"></small></div>"
    "<p><a href=\"/snapshot.jpg\">snapshot</a> | <a href=\"/status\">status</a></p>"
    "<script>"
    "var s=document.getElementById('res'),q=document.getElementById('q'),b=document.getElementById('btn'),"
    "m=document.getElementById('msg');"
    "b.onclick=function(){b.disabled=true;m.className='';m.textContent='Applying...';"
    "fetch('/camera/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'resolution='+encodeURIComponent(s.value)+'&quality='+encodeURIComponent(q.value)})"
    ".then(function(r){return r.json()}).then(function(j){"
    "if(j.resolution_id){s.value=j.resolution_id;q.value=j.jpeg_quality;"
    "document.getElementById('cur').textContent=j.name+' '+j.width+'x'+j.height+', JPEG quality '+j.jpeg_quality;}"
    "if(j.ok){m.textContent=j.saved?'Applied and saved.':'Applied, but NOT saved: '+j.error;"
    "if(!j.saved)m.className='err';}"
    "else{m.className='err';m.textContent='Failed: '+j.error+' (still using the previous settings)';}"
    "}).catch(function(){m.className='err';m.textContent='Request failed';}).then(function(){b.disabled=false;});};"
    "</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    char buf[160];
    camera_settings_t act;
    camera_get_settings(&act);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send_chunk(req, INDEX_HEAD, HTTPD_RESP_USE_STRLEN);
    snprintf(buf, sizeof(buf), "%s %ux%u, JPEG quality %u", act.resolution->name, (unsigned)act.resolution->width,
             (unsigned)act.resolution->height, (unsigned)act.quality);
    if (r == ESP_OK) {
        r = httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    }
    if (r == ESP_OK) {
        r = httpd_resp_send_chunk(req, INDEX_MID, HTTPD_RESP_USE_STRLEN);
    }
    for (int i = 0; i < CAMERA_RESOLUTION_COUNT && r == ESP_OK; i++) {
        const camera_resolution_t *c = camera_resolution_at(i);
        snprintf(buf, sizeof(buf), "<option value=\"%u\"%s>%s %ux%u</option>", (unsigned)c->id,
                 c == act.resolution ? " selected" : "", c->name, (unsigned)c->width, (unsigned)c->height);
        r = httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    }
    if (r == ESP_OK) {
        r = httpd_resp_send_chunk(req, INDEX_QUALITY, HTTPD_RESP_USE_STRLEN);
    }
    if (r == ESP_OK) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)act.quality);
        r = httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    }
    if (r == ESP_OK) {
        r = httpd_resp_send_chunk(req, INDEX_TAIL, HTTPD_RESP_USE_STRLEN);
    }
    if (r == ESP_OK) {
        if (!camera_settings_persistent()) {
            /* placed after the page: the script above only reads the DOM on click */
            r = httpd_resp_send_chunk(req, "<script>document.getElementById('per').textContent="
                                           "'Persistent storage unavailable: the selection resets on reboot.';</script>",
                                      HTTPD_RESP_USE_STRLEN);
        }
    }
    if (r == ESP_OK) {
        r = httpd_resp_send_chunk(req, NULL, 0);
    }
    return r;
}

#define SETTINGS_BODY_MAX 40

/* Parse "<key>=<1-3 digits>" at *p, advance past it; false if malformed */
static bool parse_field(const char **p, const char *key, int *value)
{
    size_t kl = strlen(key);
    if (strncmp(*p, key, kl) != 0 || (*p)[kl] != '=') {
        return false;
    }
    const char *c = *p + kl + 1;
    int v = 0, digits = 0;
    while (*c >= '0' && *c <= '9') {
        if (++digits > 3) {
            return false;
        }
        v = v * 10 + (*c++ - '0');
    }
    if (digits == 0) {
        return false;
    }
    *value = v;
    *p = c;
    return true;
}

/* POST /camera/settings  body "resolution=<id>&quality=<n>"; only supported resolution ids and the documented quality
 * range are accepted */
static esp_err_t settings_handler(httpd_req_t *req)
{
    char body[SETTINGS_BODY_MAX + 1];
    char json[256];
    int res = 0, quality = 0;
    bool saved = false;
    const char *status = "200 OK";
    const char *err_text = "";
    esp_err_t e = ESP_ERR_INVALID_ARG;

    if (req->content_len == 0 || req->content_len > SETTINGS_BODY_MAX) {
        status = "400 Bad Request";
        err_text = "invalid request body";
    } else {
        size_t got = 0;
        while (got < req->content_len) {
            int n = httpd_req_recv(req, body + got, req->content_len - got);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            got += n;
        }
        if (got != req->content_len) {
            return ESP_FAIL;
        }
        body[got] = 0;
        const char *p = body;
        if (!parse_field(&p, "resolution", &res) || *p++ != '&' || !parse_field(&p, "quality", &quality) || *p != 0) {
            status = "400 Bad Request";
            err_text = "expected resolution=<id>&quality=<n>";
        } else if (!camera_resolution_by_id(res)) {
            status = "400 Bad Request";
            err_text = "unsupported resolution";
        } else if (!camera_quality_valid(quality)) {
            status = "400 Bad Request";
            err_text = "quality out of range (" CAM_STR(CAMERA_QUALITY_MIN) "-" CAM_STR(CAMERA_QUALITY_MAX) ")";
        } else {
            e = camera_set_settings(res, quality, &saved);
        }
    }

    if (*err_text == 0) {
        if (e == ESP_OK) {
            err_text = saved ? "" : (camera_settings_persistent() ? "saving to NVS failed" : "persistent storage unavailable");
        } else if (e == ESP_ERR_TIMEOUT) {
            status = "503 Service Unavailable";
            err_text = "camera busy, try again";
        } else if (e == ESP_ERR_INVALID_STATE) {
            status = "503 Service Unavailable";
            err_text = "camera not running";
        } else if (e == ESP_ERR_INVALID_RESPONSE) {
            status = "500 Internal Server Error";
            err_text = "camera did not deliver the new frame size";
        } else {
            status = "500 Internal Server Error";
            err_text = "camera rejected the setting";
        }
    }

    camera_settings_t act;
    camera_get_settings(&act);
    int n = snprintf(json, sizeof(json),
                     "{\"ok\":%s,\"saved\":%s,\"resolution_id\":%u,\"name\":\"%s\",\"width\":%u,\"height\":%u,"
                     "\"jpeg_quality\":%u,\"error\":\"%s\"}",
                     e == ESP_OK ? "true" : "false", saved ? "true" : "false", (unsigned)act.resolution->id,
                     act.resolution->name, (unsigned)act.resolution->width, (unsigned)act.resolution->height,
                     (unsigned)act.quality, err_text);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, (n > 0 && n < (int)sizeof(json)) ? n : 0);
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
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
    wifi_sta_diagnostics_t wifi;
    uint64_t tx_frames;
    uint64_t tx_jpeg_bytes;
    char ip[16];
    static char buf[1536];   /* handlers run one at a time on the httpd task */
    camera_settings_t cs;
    camera_get_settings(&cs);
    frame_store_get_stats(&st);
    wifi_sta_ip_str(ip, sizeof(ip));
    wifi_sta_get_diagnostics(&wifi);
    portENTER_CRITICAL(&s_tx_stats_mux);
    tx_frames = s_tx_frames;
    tx_jpeg_bytes = s_tx_jpeg_bytes;
    portEXIT_CRITICAL(&s_tx_stats_mux);
    int n = snprintf(buf, sizeof(buf),
        "{\"fps\":%.1f,\"width\":%u,\"height\":%u,\"last_frame_bytes\":%u,"
        "\"max_frame_bytes\":%u,\"frame_max_bytes\":%u,\"frame_size\":%d,\"jpeg_quality\":%d,"
        "\"resolution_id\":%u,\"resolution_name\":\"%s\",\"resolution_width\":%u,\"resolution_height\":%u,\"settings_persistent\":%s,"
        "\"frames\":%u,\"bad_frames\":%u,\"dropped_frames\":%u,\"clients\":%d,"
        "\"tx_frames\":%llu,\"tx_jpeg_bytes\":%llu,"
        "\"wifi\":{\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"connected\":%s,\"associated\":%s,"
        "\"ap_info_valid\":%s,\"bssid\":\"%s\",\"channel\":%d,\"secondary_channel\":%d,\"secondary_offset\":\"%s\","
        "\"phy\":\"%s\",\"ap_phy_11b\":%s,\"ap_phy_11g\":%s,\"ap_phy_11n\":%s,"
        "\"bandwidth_mhz\":%d,\"power_save\":\"%s\",\"associations\":%u,\"disconnects\":%u,"
        "\"last_disconnect_reason\":%d},"
        "\"heap_free\":%u,\"heap_min_free\":%u,\"heap_largest_block\":%u,\"uptime_s\":%u}",
        st.fps, st.width, st.height, (unsigned)st.last_size,
        (unsigned)st.max_size, (unsigned)cs.resolution->max_bytes, cs.resolution->frame_size, (int)cs.quality,
        (unsigned)cs.resolution->id, cs.resolution->name, (unsigned)cs.resolution->width, (unsigned)cs.resolution->height,
        camera_settings_persistent() ? "true" : "false",
        (unsigned)st.frames, (unsigned)st.bad, (unsigned)st.dropped, (int)atomic_load(&s_clients),
        (unsigned long long)tx_frames, (unsigned long long)tx_jpeg_bytes,
        WIFI_SSID, wifi.rssi, ip, wifi.connected ? "true" : "false", wifi.associated ? "true" : "false",
        wifi.ap_info_valid ? "true" : "false", wifi.bssid, wifi.primary_channel,
        wifi.secondary_channel, wifi.secondary_offset,
        wifi.phy, wifi.ap_11b ? "true" : "false", wifi.ap_11g ? "true" : "false",
        wifi.ap_11n ? "true" : "false", wifi.bandwidth_mhz, wifi.power_save,
        (unsigned)wifi.associations, (unsigned)wifi.disconnects, wifi.last_disconnect_reason,
        (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
        (unsigned)(esp_timer_get_time() / 1000000));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (n < 0 || n >= (int)sizeof(buf)) {
        memcpy(buf, "{}", 2);
        n = 2;
    }
    return httpd_resp_send(req, buf, n);
}

static void stream_task(void *arg)
{
    httpd_req_t *req = arg;
    uint32_t last_seq = 0;
    char hdr[160];

    int fd = httpd_req_to_sockfd(req);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" BOUNDARY) != ESP_OK) {
        goto out;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    int idle_ms = 0;
    while (!atomic_load(&s_stop_stream)) {
        frame_slot_t *f = frame_store_acquire_latest(last_seq, 500);
        if (!f) {
            /* no new frame yet (first frame / camera restarting): a keepalive detects clients that went away */
            idle_ms += 500;
            if (idle_ms >= 5000) {
                idle_ms = 0;
                if (httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
                    break;
                }
            }
            continue;
        }
        idle_ms = 0;
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
        if (r == ESP_OK) {
            portENTER_CRITICAL(&s_tx_stats_mux);
            s_tx_frames++;
            s_tx_jpeg_bytes += f->size;
            portEXIT_CRITICAL(&s_tx_stats_mux);
        }
        frame_store_release(f);
        if (r != ESP_OK) {
            break; /* client went away or too slow (send timeout) */
        }
    }
out:
    httpd_resp_send_chunk(req, NULL, 0); /* best effort end of chunked body */
    httpd_req_async_handler_complete(req);
    ESP_LOGI(TAG, "stream client left (%d active), stack hwm %u", atomic_fetch_sub(&s_clients, 1) - 1,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (atomic_load(&s_clients) >= MAX_STREAM_CLIENTS) {
        /* a new viewer replaces the old (possibly stale) one: ask it to leave and wait for it */
        atomic_store(&s_stop_stream, true);
        for (int i = 0; i < 40 && atomic_load(&s_clients) > 0; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (atomic_load(&s_clients) > 0) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            return httpd_resp_send(req, "busy\n", HTTPD_RESP_USE_STRLEN);
        }
    }
    atomic_store(&s_stop_stream, false);
    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        return ESP_FAIL;
    }
    atomic_fetch_add(&s_clients, 1);
    if (xTaskCreate(stream_task, "stream", STREAM_TASK_STACK, copy, 5, NULL) != pdPASS) {
        atomic_fetch_sub(&s_clients, 1);
        httpd_req_async_handler_complete(copy);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "stream client joined (%d active)", (int)atomic_load(&s_clients));
    return ESP_OK;
}

void http_stream_drop_client(void)
{
    atomic_store(&s_stop_stream, true);
}

int http_stream_clients(void)
{
    return atomic_load(&s_clients);
}

void http_stream_start(void)
{
    if (s_server) {
        return;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    /* Allow one stream plus another HTTP request. httpd uses 3 sockets internally, so
     * LWIP_MAX_SOCKETS (sdkconfig.defaults) must be >= 3 + max_open_sockets. */
    cfg.max_open_sockets = 2;
    cfg.max_uri_handlers = 6;
    cfg.max_resp_headers = 5;
    cfg.lru_purge_enable = true;  /* a new connection replaces the (stale) one */
    cfg.send_wait_timeout = 10;   /* tolerate brief stalls (EAGAIN), drop clients that stall longer */
    cfg.recv_wait_timeout = 10;
    cfg.stack_size = 4096;        /* check the "stack hwm" values in the periodic log before lowering */

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET, .handler = index_handler },
        { .uri = "/stream",       .method = HTTP_GET, .handler = stream_handler },
        { .uri = "/snapshot.jpg", .method = HTTP_GET, .handler = snapshot_handler },
        { .uri = "/status",       .method = HTTP_GET, .handler = status_handler },
        { .uri = "/camera/settings", .method = HTTP_POST, .handler = settings_handler },
        { .uri = "/favicon.ico",  .method = HTTP_GET, .handler = favicon_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(s_server, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed", uris[i].uri);
        }
    }
    ESP_LOGI(TAG, "HTTP server on port %d", HTTP_PORT);
}
