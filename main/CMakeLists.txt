idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES usb esp_wifi mdns esp_netif freertos nvs_flash
    REQUIRES esp_http_server
)
