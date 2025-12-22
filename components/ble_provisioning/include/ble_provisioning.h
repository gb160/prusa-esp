#ifndef BLE_PROVISIONING_H
#define BLE_PROVISIONING_H

#include "esp_err.h"
#include <stdbool.h>  // Add this line!

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *device_name;
    const char *pop;
    bool reset_provisioned;
} ble_prov_config_t;

esp_err_t ble_prov_start(ble_prov_config_t *config);
bool ble_prov_is_provisioned(void);
esp_err_t ble_prov_reset(void);

#ifdef __cplusplus
}
#endif

#endif
