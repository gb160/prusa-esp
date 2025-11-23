idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES usb esp_wifi mdns esp_netif freertos nvs_flash esp_http_client esp-tls
    REQUIRES esp_http_server esp_http_client
)