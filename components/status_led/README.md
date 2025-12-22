# ESP32-S3 Status LED Component

A reusable, non-blocking RGB LED status indicator component for ESP32-S3.

## Features
- ✨ Smooth fade-in/fade-out animations
- 🎨 13 predefined colors
- 💡 Adjustable brightness (0-100%)
- ⚡ Non-blocking (runs in background FreeRTOS task)
- 🎯 Simple one-line API

## Quick Start

```c
#include "status_led.h"

void app_main(void) {
    // Initialize on GPIO 48
    status_led_init(48);
    
    // Set solid green at 50% brightness
    status_led_set(LED_GREEN, 50);
    
    // Blink blue (1 second on, 1 second off)
    status_led_blink(LED_BLUE, 100, 1000, 1000);
    
    // Turn off
    status_led_off();
}
```

## Available Colors
LED_RED, LED_GREEN, LED_BLUE, LED_YELLOW, LED_CYAN, LED_MAGENTA, 
LED_WHITE, LED_ORANGE, LED_PURPLE, LED_PINK, LED_LIME, LED_WARM_WHITE

## API Reference

### `status_led_init(int gpio_num)`
Initialize the LED component (call once at startup).

### `status_led_set(led_color_t color, uint8_t brightness)`
Set LED to a solid color.

### `status_led_blink(led_color_t color, uint8_t brightness, uint32_t on_time_ms, uint32_t off_time_ms)`
Make the LED blink with smooth animation.

### `status_led_off()`
Turn off the LED.

## Requirements
- ESP-IDF v5.0 or later
- ESP32-S3 with WS2812 RGB LED on GPIO 48
