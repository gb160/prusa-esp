idf_component_register(
    SRCS "ble_provisioning.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        nvs_flash 
        esp_wifi 
        esp_event 
        esp_netif
        bt
        wifi_provisioning
        qrcode
)
