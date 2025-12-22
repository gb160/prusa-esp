#include "status_led.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "STATUS_LED";

#define LED_STRIP_RMT_RES_HZ (10 * 1000 * 1000) // 10MHz resolution

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

static const rgb_t color_map[] = {
    [OFF] = {0, 0, 0},
    [RED] = {255, 0, 0},
    [GREEN] = {0, 255, 0},
    [BLUE] = {0, 0, 255},
    [YELLOW] = {255, 255, 0},
    [CYAN] = {0, 255, 255},
    [MAGENTA] = {255, 0, 255},
    [WHITE] = {255, 255, 255},
    [ORANGE] = {255, 165, 0},
    [PURPLE] = {128, 0, 128},
    [PINK] = {255, 192, 203},
    [LIME] = {191, 255, 0},
    [WARM_WHITE] = {255, 230, 180}
};

typedef struct {
    rmt_channel_handle_t led_chan;
    rmt_encoder_handle_t led_encoder;
    bool initialized;
    bool blinking;
    TaskHandle_t blink_task;
    led_color_t current_color;
    uint8_t brightness;
    uint32_t on_time;
    uint32_t off_time;
} status_led_ctx_t;

static status_led_ctx_t led_ctx = {0};

static void apply_brightness(rgb_t *rgb, uint8_t brightness) {
    if (brightness > 100) brightness = 100;
    rgb->r = (rgb->r * brightness) / 100;
    rgb->g = (rgb->g * brightness) / 100;
    rgb->b = (rgb->b * brightness) / 100;
}

static void set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t led_data[3] = {g, r, b}; // WS2812 uses GRB order
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    ESP_ERROR_CHECK(rmt_transmit(led_ctx.led_chan, led_ctx.led_encoder, led_data, sizeof(led_data), &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_ctx.led_chan, 100));
}

static void blink_task(void *param) {
    rgb_t base_color = color_map[led_ctx.current_color];
    const int fade_steps = 50;
    const int fade_delay_ms = 10;

    while (led_ctx.blinking) {
        // Fade in
        for (int i = 0; i <= fade_steps && led_ctx.blinking; i++) {
            float factor = sinf((float)i / fade_steps * M_PI / 2); // Smooth ease-in
            rgb_t rgb = base_color;
            rgb.r = (uint8_t)(rgb.r * factor);
            rgb.g = (uint8_t)(rgb.g * factor);
            rgb.b = (uint8_t)(rgb.b * factor);
            apply_brightness(&rgb, led_ctx.brightness);
            set_rgb(rgb.r, rgb.g, rgb.b);
            vTaskDelay(pdMS_TO_TICKS(fade_delay_ms));
        }

        // Stay on
        if (led_ctx.blinking) {
            vTaskDelay(pdMS_TO_TICKS(led_ctx.on_time - (fade_steps * fade_delay_ms)));
        }

        // Fade out
        for (int i = fade_steps; i >= 0 && led_ctx.blinking; i--) {
            float factor = sinf((float)i / fade_steps * M_PI / 2);
            rgb_t rgb = base_color;
            rgb.r = (uint8_t)(rgb.r * factor);
            rgb.g = (uint8_t)(rgb.g * factor);
            rgb.b = (uint8_t)(rgb.b * factor);
            apply_brightness(&rgb, led_ctx.brightness);
            set_rgb(rgb.r, rgb.g, rgb.b);
            vTaskDelay(pdMS_TO_TICKS(fade_delay_ms));
        }

        // Stay off
        if (led_ctx.blinking) {
            vTaskDelay(pdMS_TO_TICKS(led_ctx.off_time - (fade_steps * fade_delay_ms)));
        }
    }
    
    led_ctx.blink_task = NULL;
    vTaskDelete(NULL);
}

int status_led_init(int gpio_num) {
    if (led_ctx.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return 0;
    }

    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = gpio_num,
        .mem_block_symbols = 64,
        .resolution_hz = LED_STRIP_RMT_RES_HZ,
        .trans_queue_depth = 4,
    };
    
    if (rmt_new_tx_channel(&tx_chan_config, &led_ctx.led_chan) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT TX channel");
        return -1;
    }

    led_strip_encoder_config_t encoder_config = {
        .resolution = LED_STRIP_RMT_RES_HZ,
    };
    
    if (rmt_new_led_strip_encoder(&encoder_config, &led_ctx.led_encoder) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip encoder");
        return -1;
    }

    if (rmt_enable(led_ctx.led_chan) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RMT channel");
        return -1;
    }

    led_ctx.initialized = true;
    led_ctx.blinking = false;
    led_ctx.blink_task = NULL;
    
    set_rgb(0, 0, 0); // Turn off initially
    
    ESP_LOGI(TAG, "Initialized on GPIO %d", gpio_num);
    return 0;
}

void status_led_set(led_color_t color, uint8_t brightness) {
    if (!led_ctx.initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return;
    }

    // Stop blinking if active and wait for task to exit
    if (led_ctx.blinking) {
        led_ctx.blinking = false;
        while (led_ctx.blink_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    rgb_t rgb = color_map[color];
    apply_brightness(&rgb, brightness);
    set_rgb(rgb.r, rgb.g, rgb.b);
    
    ESP_LOGI(TAG, "Set to color %d, brightness %d%%", color, brightness);
}

void status_led_blink(led_color_t color, uint8_t brightness, uint32_t on_time_ms, uint32_t off_time_ms) {
    if (!led_ctx.initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return;
    }

    // Stop existing blink task and wait for it to fully exit
    if (led_ctx.blinking) {
        led_ctx.blinking = false;
        // Wait for the task to actually terminate
        while (led_ctx.blink_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    led_ctx.current_color = color;
    led_ctx.brightness = brightness;
    led_ctx.on_time = on_time_ms;
    led_ctx.off_time = off_time_ms;
    led_ctx.blinking = true;

    xTaskCreate(blink_task, "led_blink", 2048, NULL, 5, &led_ctx.blink_task);
    
    ESP_LOGI(TAG, "Blinking color %d at %d%%, %lums on / %lums off", color, brightness, on_time_ms, off_time_ms);
}

void status_led_off(void) {
    status_led_set(OFF, 0);
}

void status_led_deinit(void) {
    if (!led_ctx.initialized) {
        return;
    }

    if (led_ctx.blinking) {
        led_ctx.blinking = false;
        if (led_ctx.blink_task) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    set_rgb(0, 0, 0);
    
    rmt_disable(led_ctx.led_chan);
    rmt_del_encoder(led_ctx.led_encoder);
    rmt_del_channel(led_ctx.led_chan);
    
    led_ctx.initialized = false;
    ESP_LOGI(TAG, "Deinitialized");
}