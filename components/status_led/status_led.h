#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Predefined colors
typedef enum {
    OFF = 0,
    RED,
    GREEN,
    BLUE,
    YELLOW,
    CYAN,
    MAGENTA,
    WHITE,
    ORANGE,
    PURPLE,
    PINK,
    LIME,
    WARM_WHITE
} led_color_t;

/**
 * @brief Initialize the status LED component
 * 
 * @param gpio_num GPIO number for the RGB LED (default: 48 for ESP32-S3)
 * @return 0 on success, -1 on failure
 */
int status_led_init(int gpio_num);

/**
 * @brief Set LED to a solid color
 * 
 * @param color Predefined color from led_color_t enum
 * @param brightness Brightness percentage (0-100)
 */
void status_led_set(led_color_t color, uint8_t brightness);

/**
 * @brief Set LED to blink with smooth animation
 * 
 * @param color Predefined color from led_color_t enum
 * @param brightness Maximum brightness percentage (0-100)
 * @param on_time_ms Time LED stays fully on (milliseconds)
 * @param off_time_ms Time LED stays fully off (milliseconds)
 */
void status_led_blink(led_color_t color, uint8_t brightness, uint32_t on_time_ms, uint32_t off_time_ms);

/**
 * @brief Turn off the LED
 */
void status_led_off(void);

/**
 * @brief Deinitialize the status LED and free resources
 */
void status_led_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // STATUS_LED_H