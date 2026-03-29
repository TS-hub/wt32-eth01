idf_component_register(
    SRCS "wifi_config.c" "iperf3_server.c" "main.c" "oled.c"
    INCLUDE_DIRS "."
    REQUIRES freertos nvs_flash esp_netif esp_eth esp_event esp_system console json esp_hw_support esp_wifi esp_http_server esp_driver_i2c
)
