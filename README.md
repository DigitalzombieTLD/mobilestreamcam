# esp32c3_wifi_stream (AI-Thinker ESP32-CAM)

ESP-IDF firmware (v5.2+, CI uses v5.2.2) for the common **AI-Thinker style ESP32-CAM** (ESP32-S, 4 MB flash, 4 MB PSRAM,
OV2640). It captures JPEG frames from the onboard OV2640 and serves them over Wi-Fi as before: `/`, `/stream`
(MJPEG), `/snapshot.jpg`, `/status`, `/favicon.ico` (-> 204). The directory name is historical (the project used to
run on a XIAO ESP32-C3 receiving frames from the HX6538 over SPI; that receive path has been removed from this build
so its GPIOs and SPI DMA buffers cannot conflict with the camera).

**Status: this migration and the quality/resolution upgrade have been written but not built or tested on hardware.**
No ESP-IDF toolchain or board was available. Numbers in the RAM section below date from the old C3 design.

## Build and flash

```bash
cd esp32c3_wifi_stream
rm -f sdkconfig sdkconfig.old && idf.py fullclean   # a generated sdkconfig overrides sdkconfig.defaults!
idf.py set-target esp32 && idf.py build             # downloads espressif/esp32-camera via the component manager
idf.py -p /dev/ttyUSB0 flash monitor
```

Wi-Fi credentials / static IP: copy overrides into `main/config_local.h` (git-ignored), see `main/config.h`.

### ESP32-CAM-MB programming adapter
* Plug the ESP32-CAM into the ESP32-CAM-MB board (USB-C/micro-USB, CH340 UART bridge; serial port is usually
  `/dev/ttyUSB0` or `COMx`). The console is UART0 at 115200 baud.
* The MB adapter drives GPIO0 and EN automatically, so normally no manual jumper is needed. If flashing fails with
  "Failed to connect", hold the **IO0** button on the adapter, tap **RST**, then release IO0 (download mode), or retry.
* After flashing press **RST** (or re-plug) to boot the app. GPIO0 is also the camera XCLK pin: it must not be held
  low at reset, otherwise the chip stays in download mode (prints `waiting for download`).
* The ROM bootloader prints some garbage at 74880 baud after reset; this is normal.
* A weak USB port/cable can cause brownout resets when Wi-Fi starts; use a good 5 V supply.

### Camera pin map
`main/config.h` contains the standard AI-Thinker OV2640 map (PWDN=32, XCLK=0, SIOD=26, SIOC=27, D0-D7=5,18,19,21,36,39,
34,35, VSYNC=25, HREF=23, PCLK=22). All `CAM_PIN_*` values, frame size (`CAM_FRAME_SIZE`, default SVGA) and JPEG quality
(`CAM_JPEG_QUALITY`, default 10) can be overridden in `main/config_local.h`. **Other ESP32-CAM variants (M5Stack, TTGO, Wrover-Kit, ...) need a different
pin map.** GPIO4 (flash LED) and the SD card pins are not used.

### Design
* `camera.c` initialises the OV2640 (JPEG, `CAMERA_GRAB_LATEST`, 2 frame buffers in PSRAM), copies each frame into a
  frame-store slot and returns the driver buffer immediately, so a slow HTTP client never holds a camera buffer.
  The copy (PSRAM to PSRAM, a few ms) is kept on purpose: it guarantees latest-frame semantics and lets the second
  driver buffer capture the next frame while the previous one is copied.
* `frame_store.c` keeps 3 slots of `FRAME_MAX_BYTES` (128 KB by default, set by the preset) in PSRAM (`CONFIG_SPIRAM=y`, quad mode, caps-alloc only:
  Wi-Fi/lwIP stay in internal RAM). Larger frames are dropped and counted as bad.
* `http_stream.c`, `wifi_sta.c` and the config mechanism are unchanged.

## Image quality, resolution and FPS

**Default: SVGA 800x600, JPEG quality 10** (lower number = better quality / bigger frames; was VGA 640x480, quality 12).
Typical SVGA q10 frames are roughly 30-60 KB. **No hardware was available, so no FPS has been measured.** The default
is chosen because the OV2640 at 20 MHz XCLK can deliver SVGA JPEG at well above 15 fps and ~40 KB x 12 fps is about
4 Mbit/s of Wi-Fi traffic, so 10-15 fps is the expectation, not a verified result. The real limit is usually Wi-Fi
(signal, router, one client) and scene complexity (noisy/detailed scenes give bigger JPEGs).

Select a preset in `main/config_local.h` (git-ignored) and rebuild:

```c
#define CAM_PRESET 3   /* 1 SVGA q10 (default), 2 VGA q12, 3 XGA q12, 4 SXGA q14, 5 UXGA q16 */
```

| Preset | Resolution | Quality | `FRAME_MAX_BYTES` | Expected |
|---|---|---|---|---|
| 1 (default) | 800x600 | 10 | 128 KB | best balance, target >= 10-15 fps (unmeasured) |
| 2 | 640x480 | 12 | 96 KB | fastest, previous default |
| 3 | 1024x768 | 12 | 160 KB | sharper, may fall below 10 fps on weak Wi-Fi |
| 4 | 1280x1024 | 14 | 224 KB | slow, probably < 8 fps |
| 5 | 1600x1200 | 16 | 320 KB | slowest, a few fps |

`CAM_FRAME_SIZE`, `CAM_JPEG_QUALITY`, `FRAME_MAX_BYTES`, `FRAME_SLOT_COUNT`, `CAM_FB_COUNT` and `CAM_XCLK_FREQ_HZ` can
also be overridden individually. Trade-offs: higher resolution / lower quality number -> larger frames -> lower fps and
more Wi-Fi load; higher quality number -> blockier image but faster. Frames larger than `FRAME_MAX_BYTES` are dropped
(counted in `bad_frames`) and never written past the slot; a compile-time check keeps slots + camera buffers under 3 MB of
the 4 MB PSRAM. The lwIP TCP send buffer / window were raised to 23360 / 11680 bytes (internal RAM, ~12 KB more per
socket) so a 100 KB frame does not need many round trips. Delete `sdkconfig` after changing `sdkconfig.defaults`.

### Checking the result with `/status`
1. Start the stream in a viewer, wait ~10 s, then open `http://<ip>/status` from the same or another device (it replaces
   the stream connection, reopen `/` afterwards). `fps` decays to 0 after 3 s without new frames.
2. `fps` - frames per second captured over the last second. Goal >= 10-15.
3. `width` / `height` / `jpeg_quality` / `frame_size` - the active settings. `last_frame_bytes` and `max_frame_bytes`
   - actual JPEG size and the largest seen; `max_frame_bytes` should stay well below `frame_max_bytes` (the slot size).
4. `bad_frames` increasing -> frames exceeded `frame_max_bytes` (or were not valid JPEG): raise `FRAME_MAX_BYTES`
   or the quality number. `dropped_frames` increasing -> no free slot (the HTTP side is too slow): lower resolution/quality
   or improve Wi-Fi (`wifi.rssi`, better than about -70 dBm).
5. `heap_free` / `heap_min_free` / `heap_largest_block` should stay stable over minutes (no leak).
If `fps` < 10, go one preset down or raise `CAM_JPEG_QUALITY` by 2; if `fps` is well above 15, go one preset up.

## RAM budget (historical, from the ESP32-C3 design; the Wi-Fi/lwIP trimming is kept)

Problem on hardware (old config): `/status` after 30 s showed `heap_free 8412`, `heap_min_free 4284`; the page loaded
once, then new connections failed and the chip rebooted.

| Item | Before | After | Measured? |
|---|---|---|---|
| Frame slots (3x) | 3 x 56 KB = ~170 KB | 3 x 24 KB = ~74 KB (`FRAME_MAX_BYTES`, larger frames are dropped) | computed |
| lwIP TCP send buf / window | 23360 / 11680 per socket | 11520 / 5760 | computed |
| WiFi dynamic RX/TX buffers | 24 / 24 | 16 / 16 | computed |
| HTTP sockets | 5 (+3 internal) | 2 (+3 internal), `LWIP_MAX_SOCKETS=6` | computed |
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
`max_open_sockets = 2` with `lru_purge_enable` allows one MJPEG stream and another HTTP request (such as `/status` or a
page asset) to coexist; `MAX_STREAM_CLIENTS` remains 1. httpd uses 3 sockets internally, so `CONFIG_LWIP_MAX_SOCKETS=6`
provides 3 internal sockets, 2 client sockets and one spare. If an existing generated `sdkconfig` is present, remove it
and rebuild for the default to take effect: `rm -f sdkconfig sdkconfig.old && idf.py fullclean && idf.py build`.
Send/receive timeouts are 10 s.

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
| `LWIP_MAX_SOCKETS` | 6 | 3 httpd internal + 2 client + 1 spare | - |
| `LWIP_MAX_ACTIVE_TCP` / `LISTENING_TCP` / `UDP_PCBS` | 8 / 2 / 6 | fewer PCBs | - |
| `MSC_ENABLE_MDNS` (own option) | y | `<host>.local`; set `n` to save ~8-10 KB | no `.local` name |
| `MDNS_MAX_SERVICES` | 1 | only `_http._tcp` | - |
| `HTTPD_MAX_URI_LEN` | 128 | smaller scratch buffer | longer URIs rejected |

esp-tls / http_client / console are not linked because no component requires them.
