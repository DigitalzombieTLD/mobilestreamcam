idf_component_register(SRCS "main.c" "wifi_sta.c" "frame_store.c" "camera.c" "http_stream.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_http_server esp_netif esp_event esp_timer
                                     driver esp32-camera heap mdns lwip esp_system nvs_flash)
