# esp32c3_wifi_stream

XIAO ESP32-C3 firmware (ESP-IDF v5.4): receives MJPEG frames (640x480, ~5.7 KB each, ~16 fps) from the HX6538 over SPI
and serves them over HTTP (`/`, `/stream`, `/snapshot.jpg`, `/status`, `/favicon.ico` -> 204). Protocol and wiring:
[`docs/mjpeg_stream.md`](../docs/mjpeg_stream.md).

```bash
cd esp32c3_wifi_stream
rm -f sdkconfig sdkconfig.old && idf.py fullclean   # a generated sdkconfig overrides sdkconfig.defaults!
idf.py set-target esp32c3 && idf.py build
idf.py -p /dev/ttyACM0 flash && idf.py -p /dev/ttyACM0 monitor
```

All configuration is in `sdkconfig.defaults` (`sdkconfig` is git-ignored). **After changing the defaults, delete
`sdkconfig` and run `idf.py fullclean`**, otherwise the old values stay active.

## RAM budget

Problem on hardware (old config): `/status` after 30 s showed `heap_free 8412`, `heap_min_free 4284`; the page loaded
once, then new connections failed and the chip rebooted.

| Item | Before | After | Measured? |
|---|---|---|---|
| Frame slots (3x) | 3 x 56 KB = ~170 KB | 3 x 24 KB = ~74 KB (`FRAME_MAX_BYTES`, larger frames are dropped) | computed |
| lwIP TCP send buf / window | 23360 / 11680 per socket | 11520 / 5760 | computed |
| WiFi dynamic RX/TX buffers | 24 / 24 | 16 / 16 | computed |
| HTTP sockets | 5 (+3 internal) | 1 (+3 internal), `LWIP_MAX_SOCKETS=5` | computed |
| Stream clients / tasks | up to 3 tasks x 4 KB | 1 task x 3.5 KB | computed |
| IRAM optimizations (WiFi, lwIP) | on | off (IRAM and DRAM share the C3 SRAM) | computed |
| NVS | initialised | not used | computed |
| `heap_free` after boot | 8.4 KB (30 s) | **not measured - no hardware in the build environment**; target >= 100 KB | **could not be measured** |
| fps at 640x480 | 16 | expected unchanged (> 15); **not measured** | **could not be measured** |

Measure on hardware: `GET /status` (`heap_free`, `heap_min_free`, `heap_largest_block`) and the serial log line
`heap free ..., min ..., largest block ... | stack hwm httpd/stream/spi_rx` printed at boot and every 10 s. The
`stack hwm` values are the minimum free stack in bytes (`uxTaskGetStackHighWaterMark`); lower the stack sizes
(`cfg.stack_size`, `STREAM_TASK_STACK`, `SPI_RX_STACK_BYTES`) only if they stay well above ~512.

### How leaks are avoided
* No per-frame or per-request `malloc`/`free`: frame slots are allocated once (zero-copy ref counted hand-off from the
  SPI task to the HTTP task), `/status` and the stream headers use stack buffers, the SPI task uses a static stack.
* Only one stream task exists; every exit path of the task (client left, send error, timeout, replaced by a new
  viewer, low-heap drop) releases the frame slot, completes the async request, decrements the client counter and
  deletes the task. A new `/stream` request first asks the running one to leave.
* WiFi reconnect uses a single one-shot timer; `esp_wifi_connect()` is only called from `STA_START` and from that timer
  (stopped on connect / got-IP). The radio settings (bandwidth, TX power) are applied before connecting: changing
  the bandwidth from the `STA_CONNECTED` handler caused the `disconnected (reason 8)` right after connecting.
* Low-heap watchdog (`main.c`): free heap or largest block < 6 KB for 5 s -> the stream client is dropped; still low
  after 10 s -> `esp_restart()`. `CONFIG_ESP_SYSTEM_PANIC_REBOOT=y` also reboots after a panic.

### HTTP server
`max_open_sockets = 1` with `lru_purge_enable`: the page (`/`) and the stream (`/stream`, own task, async handler) do not
deadlock because the page connection is closed/purged when the browser opens the stream connection, and the page
itself needs no further requests. `/status` while streaming replaces the stream connection (one viewer only); use a
second request only when no stream is open. If this proves unworkable with a given browser raise `max_open_sockets`
to 2 and `CONFIG_LWIP_MAX_SOCKETS` to 6 (httpd uses 3 sockets internally). Send/receive timeouts are 10 s.

### sdkconfig changes

| Option | Value | Reason | Trade-off |
|---|---|---|---|
| `COMPILER_OPTIMIZATION_SIZE` | y | smaller code | slightly slower code (fps to be verified) |
| `ESP_SYSTEM_PANIC_REBOOT` | y | camera recovers by itself | none |
| `ESP_TASK_WDT_TIMEOUT_S` | 10 | sane watchdog | slower hang detection |
| `ESP_CONSOLE_USB_SERIAL_JTAG` | y | XIAO C3 logs go over its USB port | none |
| `HEAP_POISONING_DISABLED`, `HEAP_TRACING_OFF`, `FREERTOS_USE_TRACE_FACILITY=n` | - | no debug overhead | no heap debugging |
| `BOOTLOADER_LOG_LEVEL_WARN` | y | quieter boot | - |
| `BT_ENABLED` | n | no Bluetooth needed | - |
| `ESP_PHY_CALIBRATION_AND_DATA_STORAGE` | n | NVS no longer needed | full PHY calibration each boot (~100 ms) |
| `ESP_WIFI_STATIC_RX_BUFFER_NUM` | 8 | fewer permanent RX buffers | slightly less burst headroom |
| `ESP_WIFI_DYNAMIC_RX/TX_BUFFER_NUM` | 16 / 16 | cap WiFi buffer RAM | fewer queued packets; > 5 Mbit/s needed only |
| `ESP_WIFI_RX_BA_WIN` / `TX_BA_WIN` | 6 / 6 | smaller block-ack windows (<= dynamic RX and <= 2x static RX) | lower peak throughput |
| `ESP_WIFI_IRAM_OPT`, `RX_IRAM_OPT`, `SLP_IRAM_OPT` | n | IRAM and DRAM share the C3 SRAM | a little less WiFi throughput |
| `ESP_WIFI_SOFTAP_SUPPORT` | n | STA only | no AP mode |
| `ESP_WIFI_NVS_ENABLED` | n | WiFi config kept in RAM | none |
| `ESP_WIFI_FTM/DPP/11KV/ENTERPRISE/OWE/SAE_PK/SUITE_B_192/GCMP/GMAC`, RX/TX stats | n | unused features | - |
| `ESP_WIFI_ENABLE_WPA3_SAE` | y | the "SteamCam" network is WPA3 | a few KB of RAM and during auth; set `n` for WPA2-only networks. WPA2/WPA3 mixed works either way |
| `LWIP_IPV6` | n | no IPv6 | IPv4 only |
| `LWIP_DHCPS`, `IP_FORWARD`, `IPV4_NAPT`, `LWIP_STATS`, `BROADCAST_PING`, `DHCP_DOES_ARP_CHECK` | n | unused | - |
| `LWIP_IRAM_OPTIMIZATION`, `EXTRA_IRAM_OPTIMIZATION` | n | IRAM shares SRAM | slightly slower lwIP |
| `LWIP_TCP_SND_BUF_DEFAULT` / `WND_DEFAULT` | 11520 / 5760 | ~2 frames in flight; 4 MSS window | upload limited to roughly 8 Mbit/s at WiFi latency, enough for ~6 Mbit/s |
| `LWIP_TCP_RECVMBOX_SIZE`, `UDP_RECVMBOX_SIZE`, `TCPIP_RECVMBOX_SIZE` | 6, 6, 16 | small mailboxes | - |
| `LWIP_TCP_ACCEPTMBOX_SIZE` | 2 | one client | - |
| `LWIP_TCPIP_TASK_STACK_SIZE` | 2560 | smaller tcpip task | verify with hwm if more handlers are added |
| `LWIP_TCP_SACK_OUT` | n | saves memory | slower loss recovery |
| `LWIP_MAX_SOCKETS` | 5 | 3 httpd internal + 1 + 1 spare | - |
| `LWIP_MAX_ACTIVE_TCP` / `LISTENING_TCP` / `UDP_PCBS` | 8 / 2 / 6 | fewer PCBs | - |
| `MSC_ENABLE_MDNS` (own option) | y | `<host>.local`; set `n` to save ~8-10 KB | no `.local` name |
| `MDNS_MAX_SERVICES` | 1 | only `_http._tcp` | - |
| `HTTPD_MAX_URI_LEN` | 128 | smaller scratch buffer | longer URIs rejected |

esp-tls / http_client / console are not linked because no component requires them.
