#include "ble_provisioning.h"
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <nvs_flash.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>
#include "qrcode.h"  // ADD THIS


#define PROV_QR_VERSION "v1"
#define PROV_TRANSPORT  "ble"
#define QRCODE_BASE_URL "https://espressif.github.io/esp-jumpstart/qrcode.html"


static const char *TAG = "BLE_PROV";

static EventGroupHandle_t wifi_event_group = NULL;
#define WIFI_CONNECTED_EVENT BIT0

static void print_qr_code(const char *name, const char *pop)
{
    char payload[150];
    snprintf(payload, sizeof(payload),
        "{\"ver\":\"%s\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\"}",
        PROV_QR_VERSION, name, pop, PROV_TRANSPORT);

    ESP_LOGI(TAG, "Scan this QR code from the provisioning app:");
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    esp_qrcode_generate(&cfg, payload);
    ESP_LOGI(TAG, "If QR code is not visible, copy this URL:");
    ESP_LOGI(TAG, "%s?data=%s", QRCODE_BASE_URL, payload);
}

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_PROV_EVENT) {
        if (event_id == WIFI_PROV_START)
            ESP_LOGI(TAG, "Provisioning started");
        else if (event_id == WIFI_PROV_END)
            wifi_prov_mgr_deinit();
    } else if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START)
            esp_wifi_connect();
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
            esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void get_device_service_name(char *service_name, size_t max, const char *base_name)
{
    strncpy(service_name, base_name, max);
    service_name[max - 1] = '\0';
}

bool ble_prov_is_provisioned(void)
{
    bool provisioned = false;
    wifi_prov_mgr_is_provisioned(&provisioned);
    return provisioned;
}

esp_err_t ble_prov_reset(void)
{
    return wifi_prov_mgr_reset_provisioning();
}

esp_err_t ble_prov_start(ble_prov_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_prov_mgr_config_t prov_config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM
    };

    ESP_ERROR_CHECK(wifi_prov_mgr_init(prov_config));

    bool provisioned = false;

    if (config->reset_provisioned) {
        wifi_prov_mgr_reset_provisioning();
    }

    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    if (!provisioned) {
        char service_name[32];
        get_device_service_name(service_name, sizeof(service_name), 
                                config->device_name ? config->device_name : "ESP32_PROV");

        wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
        const char *pop = config->pop ? config->pop : "abcd1234";
        wifi_prov_security1_params_t *sec_params = (void *)pop;

        wifi_prov_mgr_start_provisioning(
            security, sec_params, service_name, NULL);

        ESP_LOGI(TAG, "Scan for: %s, PoP: %s", service_name, pop);

        print_qr_code(service_name, pop);  // ADD THIS LINE

    } else {
        wifi_prov_mgr_deinit();
        wifi_init_sta();
    }

    xEventGroupWaitBits(
        wifi_event_group, WIFI_CONNECTED_EVENT,
        true, true, portMAX_DELAY);

    return ESP_OK;
}
