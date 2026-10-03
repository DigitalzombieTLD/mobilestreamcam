# mobilestreamcam: WiFi MJPEG streaming camera

Grove Vision AI Module V2 (HX6538 + OV5647 on CSI) plus the XIAO ESP32-C3 daughterboard become a
standalone WiFi camera:

```
OV5647 --MIPI--> HX6538 (HW JPEG encoder) --SPI (HX = master)--> XIAO ESP32-C3 --WiFi--> HTTP MJPEG
```

The ESP32-C3 never transcodes: it relays the JPEG bytes produced by the HX6538 hardware encoder.

| Part | Location | Build |
| ---- | -------- | ----- |
| HX6538 firmware | `EPII_CM55M_APP_S/app/scenario_app/mjpeg_stream_spi` | GNU make + arm-none-eabi-gcc |
| ESP32-C3 firmware | `esp32c3_wifi_stream/` | ESP-IDF v5.2 (>= 5.1) |

> **Status:** both firmwares compile (HX6538 image 299 KB, limit 1 MB; ESP32-C3 image ~0.8 MB).
> **Nothing was tested on real hardware.** Items marked **UNVERIFIED** below need a hardware check, see the
> checklist at the end.

## Endpoints

| URL | Content |
| --- | ------- |
| `http://<ip>/` (or `http://mobilestreamcam.local/`) | tiny page embedding the stream |
| `http://<ip>/stream` | `multipart/x-mixed-replace` MJPEG (boundary `mobilestreamcamframe`, `Content-Length` per part) |
| `http://<ip>/snapshot.jpg` | latest frame |
| `http://<ip>/status` | JSON: fps, width/height, frame counters, wifi ssid/rssi/ip, heap, uptime |

At most 2 simultaneous `/stream` clients (`MAX_STREAM_CLIENTS`); a slow client is skipped ahead to the newest
frame (old frames are dropped), a stalled client is disconnected after 3 s.

Open the stream:

```bash
vlc http://mobilestreamcam.local/stream        # or Media > Open Network Stream
ffplay -fflags nobuffer http://<ip>/stream
```

## Wiring / pinout (on-board, nothing to wire)

The XIAO ESP32-C3 plugs into the XIAO header of the Grove Vision AI V2. SPI uses the HX6538 SPI master
(the same pins the other apps use for `hx_drv_spi_mst_protocol_write_sp`):

| Signal | HX6538 pin | XIAO pin | ESP32-C3 GPIO |
| ------ | ---------- | -------- | ------------- |
| SCLK   | PB4 (`SPI_M_SCLK_1`) | D8  | GPIO8  |
| MOSI (HX -> ESP) | PB2 (`SPI_M_DO_1`)   | D10 | GPIO10 |
| MISO (ESP -> HX) | PB3 (`SPI_M_DI_1`)   | D9  | GPIO9  |
| CS     | PB11 (`SPI_M_CS`)    | D1  | GPIO3  |

Source of the mapping: Grove Vision AI V2 schematic as tabulated in
<https://github.com/Mark-MDO47/expts_grove_vision_ai_v2/blob/master/_README_Hardware.md>
and the pin mux used by the existing apps (`spi_m_pinmux_cfg`). **UNVERIFIED on hardware.**
GPIO8/GPIO9 are ESP32-C3 strapping pins; the HX6538 holds PB3 low after reset, which could interfere with
booting the ESP32-C3 into the application (GPIO9 low = download mode). If the ESP32-C3 boots into download
mode, power-cycle again or report it, see troubleshooting.

I2C (D4/D5 = GPIO6/7) is not used by this firmware; it is only needed for commands (not implemented).

## HX6538 firmware (`mjpeg_stream_spi`)

* Based on `tflm_yolov8_od` / `allon_sensor` without any TFLM code, bare-metal (no FreeRTOS), `cis_ov5647`.
* Sensor datapath: INP -> HW5x5 (YUV) -> HW JPEG encoder (YUV420), continuous capture. Each frame is copied out
  of the sensor JPEG buffer, capture is restarted immediately, then the copy is sent with
  `hx_drv_spi_mst_protocol_write_sp(..., DATA_TYPE_JPG)`. Frames that are empty or larger than
  `MJPEG_MAX_FRAME_BYTES` are dropped instead of stalling the pipeline.
* Settings are in `app/scenario_app/mjpeg_stream_spi/common_config.h`:
  `MJPEG_RES_MODE`, `MJPEG_JPEG_QTABLE`, `MJPEG_MAX_FRAME_BYTES`, `MJPEG_SPI_CLK_HZ`, `MJPEG_STATS_PERIOD_MS`.
* **Resolution:** the OV5647 driver in this repository only has a 640x480 (binned) sensor mode and the
  datapath/JPEG limits are 640x480 (`DP_HW5X5` max crop 644x484). Higher resolutions (800x600, 1024x768) are
  therefore **not** available without writing a new OV5647 register table and datapath; 640x480 is the highest
  supported mode. 320x240 can be selected with `MJPEG_RES_MODE 1`.
* Serial output (921600 baud, 8N1) prints once a second, e.g.
  `MJPEG 640x480: 18.0 fps, avg 31000 B/frame (last 30500), sent 18, dropped 0, tx_fail 0, total 18`.
* On a datapath error (WDT, XDMA, CDM errors) the firmware resets itself.

### Build and flash

```bash
cd EPII_CM55M_APP_S
make clean && make            # APP_TYPE is already mjpeg_stream_spi
cd ../we2_image_gen_local
cp ../EPII_CM55M_APP_S/obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65/EPII_CM55M_gnu_epii_evb_WLCSP65_s.elf input_case1_secboot/
./we2_local_image_gen project_case1_blp_wlcsp.json      # macOS: ./we2_local_image_gen_macOS_arm64 ...
cd ..
python3 xmodem/xmodem_send.py --port=/dev/ttyACM0 --baudrate=921600 --protocol=xmodem \
        --file=we2_image_gen_local/output_case1_sec_wlcsp/output.img
```

No model has to be flashed. `./build_and_flash.sh [--no-flash]` does the same in one step (no models by default;
`--with-model` forces the face-recognition models). Press the reset button of the module afterwards.

## ESP32-C3 firmware (`esp32c3_wifi_stream`)

* STA mode, scans all channels for `WIFI_SSID`, connects to the strongest AP, reconnects forever, power save
  off (`WIFI_PS_NONE`), max TX power, HT20, mDNS `mobilestreamcam.local`.
* SPI slave with DMA writes straight into one of `FRAME_SLOT_COUNT` (3) internal-RAM frame slots of
  `FRAME_MAX_BYTES` (56 KB). One slot receives, one holds the latest frame, one can be sent to a client. If no
  slot is free the frame is dropped. If RAM gets short use `FRAME_SLOT_COUNT 2`.
* lwIP / WiFi buffers are tuned in `sdkconfig.defaults` (IRAM optimisations, AMPDU, bigger TCP window/send buffer).
  Expected WiFi TCP throughput of the C3 is roughly 10-15 Mbit/s; 640x480 frames of ~30 KB at 15-20 fps need
  4-5 Mbit/s.

### Configuration

Edit `esp32c3_wifi_stream/main/config.h` or (recommended, git-ignored) create
`esp32c3_wifi_stream/main/config_local.h`:

```c
#define WIFI_SSID     "MyNetwork"
#define WIFI_PASSWORD "MyPassword"
// optional
#define DEVICE_HOSTNAME "mobilestreamcam"
#define USE_STATIC_IP 1
#define STATIC_IP "192.168.1.50"
#define STATIC_GATEWAY "192.168.1.1"
#define STATIC_NETMASK "255.255.255.0"
#define STATIC_DNS "192.168.1.1"
```

SPI pins/mode and the frame limits are also in `config.h`.

### Build and flash

```bash
cd esp32c3_wifi_stream
. $IDF_PATH/export.sh              # ESP-IDF v5.2 or newer
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

or with Docker: `docker run --rm -v $PWD:/project -w /project -u $(id -u):$(id -g) -e HOME=/tmp espressif/idf:v5.2.2 idf.py set-target esp32c3 build`.
The mDNS component is downloaded from the ESP component registry during the first build.

The XIAO ESP32-C3 has to be flashed **separately** over its own USB-C port (hold BOOT while plugging in if it
is not detected). The HX6538 is flashed over the module's USB-C port with xmodem. Each chip shows up as its own
serial device: the Grove Vision AI V2 USB-C port is the HX6538 console (921600 baud, flashing and `xprintf`
output), the XIAO's USB-C port is the ESP32-C3 (native USB serial/JTAG, `idf.py monitor` log). Which `ttyACM`/COM
number is which depends on the OS: unplug one board to see which entry disappears. Power the module from only
one USB port at a time unless you know both are isolated.

## Troubleshooting

| Symptom | Check |
| ------- | ----- |
| ESP32-C3 log: "no SPI data from the HX6538 yet" | HX firmware flashed and reset? HX serial shows the fps line? SPI pins/mode (`SPI_MODE_NUM`) |
| ESP32-C3 log: "bad frame (no PTL magic)" | the hexdump shows the real header; adjust `PTL_*` in `config.h`/`spi_rx.c` and check SPI mode and `MJPEG_SPI_CLK_HZ` |
| HX tx_fail counter grows | SPI wiring, CS pin, lower `MJPEG_SPI_CLK_HZ` (e.g. 6 MHz) |
| HX dropped counter grows | frames bigger than `MJPEG_MAX_FRAME_BYTES` -> keep `JPEG_ENC_QTABLE_10X` |
| cannot connect to WiFi | 2.4 GHz network only, SSID/password, watch log "disconnected (reason N)" |
| stream stutters | check `/status` rssi and fps; use 320x240 or a closer AP; one client only |
| ESP32-C3 boots into download mode | GPIO9 (strapping pin) pulled low by the HX, power-cycle |
| `.local` name not resolved | use the IP printed on the ESP32-C3 serial log / router lists |

## Hardware test checklist (not verifiable without hardware)

- [ ] Pinout above matches the board (SCLK GPIO8, MOSI GPIO10, MISO GPIO9, CS GPIO3), strapping pin GPIO8/9 behaviour at boot.
- [ ] SPI mode (0), 12 MHz clock and that the ESP32-C3 slave never misses data (no handshake line is used).
- [ ] Wire format of `hx_drv_spi_mst_protocol_write_sp`: assumed `C0 5A | type | len (LE32) | payload` (the HX
      library is closed source). The receiver logs a hexdump of bad frames to correct this.
- [ ] Whether a frame arrives as one SPI transaction (CS low for the whole frame) or several.
- [ ] Frame rate of the OV5647 640x480 mode (>= 15 fps) and measured fps in the serial/`/status` output.
- [ ] JPEG frame size at `JPEG_ENC_QTABLE_10X` stays below 56 KB.
- [ ] WiFi throughput with 1 and 2 clients, heap headroom (`/status` `heap_min_free`).
- [ ] mDNS name resolves, reconnect after router restart.
- [ ] Not implemented: ESP32 -> HX command channel (adaptive JPEG quality); clients are throttled by frame skipping only.
