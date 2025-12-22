#ifndef LED_STRIP_ENCODER_H
#define LED_STRIP_ENCODER_H

#include "driver/rmt_encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t resolution;
} led_strip_encoder_config_t;

esp_err_t rmt_new_led_strip_encoder(const led_strip_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder);

#ifdef __cplusplus
}
#endif

#endif // LED_STRIP_ENCODER_H
