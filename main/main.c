/*
 * Prusa Core One ESP32 Monitor V3.0 - WebSocket Edition
 * 
 * Major rewrite to fix message truncation issues and improve architecture
 * 
 * V3.0 Changes:
 * - WebSocket server for real-time bidirectional communication
 * - ALL parsing moved to ESP32 (server-side)
 * - Structured JSON messages sent to clients
 * - Per-client message queues (no global ring buffer race conditions)
 * - Eliminated HTTP polling (push-based updates)
 * - Better error handling and connection management
 * 
 * Preserved Features:
 * - USB CDC communication with printer
 * - Remote HTML loading from GitHub (with embedded fallback)
 * - G-code command sending via web interface
 * - WiFi connectivity with mDNS (coreone.local)
 * - Status LED support
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ESP32 System includes
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"

// FreeRTOS includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// USB Host includes
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

// WiFi and networking includes
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mdns.h"

// GPIO for status LED
#include "driver/gpio.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

// Prusa Core One USB identifiers
#define PRUSA_USB_VID               (0x2C99)
#define PRUSA_USB_PID               (0x001F)

// USB communication settings
#define USB_HOST_TASK_PRIORITY      (20)
#define USB_TX_TIMEOUT_MS           (1000)
#define INITIAL_BEEP_COMMAND        ("M300 S2000 P50\n")

// WiFi credentials
#define WIFI_SSID                   "BT-WXF9FJ"
#define WIFI_PASS                   "QFLQCPDLWF"


// Remote HTML configuration
#define ENABLE_REMOTE_HTML          (1)
#define REMOTE_HTML_URL             "https://raw.githubusercontent.com/gb160/prusa-esp/main/main/webpage_remote.html"

// Status LED GPIO (adjust for your ESP32-S3 SuperMini)
#define STATUS_LED_GPIO             (GPIO_NUM_48)  // Built-in LED on most ESP32-S3
#define LED_BLINK_DISCONNECTED_MS   (1000)
#define LED_BLINK_CONNECTED_MS      (100)

// WebSocket message queue configuration
#define WS_MAX_CLIENTS              (4)
#define WS_MESSAGE_QUEUE_SIZE       (50)
#define WS_MAX_PAYLOAD_SIZE         (512)

// Serial parsing buffer
#define SERIAL_LINE_BUFFER_SIZE     (512)

static const char *TAG = "PRUSA-WS-V3";

// ============================================================================
// MESSAGE TYPES AND STRUCTURES
// ============================================================================

typedef enum {
    MSG_TYPE_TEMPERATURE,
    MSG_TYPE_PROGRESS,
    MSG_TYPE_POSITION,
    MSG_TYPE_LOG,
    MSG_TYPE_STATUS,
    MSG_TYPE_POWER,
    MSG_TYPE_ERROR
} message_type_t;

typedef struct {
    message_type_t type;
    char json_payload[WS_MAX_PAYLOAD_SIZE];
} ws_message_t;

typedef struct {
    int fd;                                    // WebSocket file descriptor
    bool active;                               // Is this slot active?
    QueueHandle_t message_queue;               // Per-client message queue
} ws_client_t;

// Temperature state
typedef struct {
    float nozzle_current;
    float nozzle_target;
    float bed_current;
    float bed_target;
    float heatbreak_current;
    float heatbreak_target;
    float chamber_current;
} temp_state_t;

// Progress state
typedef struct {
    int percent;
    int time_left_mins;
    int change_mins;
} progress_state_t;

// Position state
typedef struct {
    float x;
    float y;
    float z;
    float e;
} position_state_t;

// Power state
typedef struct {
    int nozzle_pwm;
    int bed_pwm;
    int heatbreak_pwm;
} power_state_t;

// ============================================================================
// GLOBAL STATE
// ============================================================================

// Synchronization primitives
static SemaphoreHandle_t device_disconnected_sem;
static SemaphoreHandle_t html_mutex;
static SemaphoreHandle_t ws_clients_mutex;
static SemaphoreHandle_t printer_state_mutex;

// USB device handle
static cdc_acm_dev_hdl_t g_prusa_dev = NULL;
static bool initial_chirp_sent = false;

// WebSocket clients
static ws_client_t ws_clients[WS_MAX_CLIENTS];
static httpd_handle_t server = NULL;

// Printer state (protected by printer_state_mutex)
static temp_state_t current_temps = {0};
static progress_state_t current_progress = {0};
static position_state_t current_position = {0};
static power_state_t current_power = {0};
static bool printer_connected = false;

// Serial line buffer for parsing
static char serial_line_buffer[SERIAL_LINE_BUFFER_SIZE];
static size_t serial_line_pos = 0;

// LED task handle
static TaskHandle_t led_task_handle = NULL;

// Embedded webpage (fallback)
extern const uint8_t webpage_start[] asm("_binary_webpage_html_start");
extern const uint8_t webpage_end[] asm("_binary_webpage_html_end");

// Remote HTML cache
static char *cached_html = NULL;
static size_t cached_html_size = 0;
static char last_download_error[256] = "Not attempted yet";

// ============================================================================
// STATUS LED CONTROL
// ============================================================================

static void led_task(void *arg)
{
    gpio_reset_pin(STATUS_LED_GPIO);
    gpio_set_direction(STATUS_LED_GPIO, GPIO_MODE_OUTPUT);
    
    bool led_state = false;
    
    while (1) {
        if (printer_connected) {
            // Fast blink when connected
            led_state = !led_state;
            gpio_set_level(STATUS_LED_GPIO, led_state);
            vTaskDelay(pdMS_TO_TICKS(LED_BLINK_CONNECTED_MS));
        } else {
            // Slow blink when disconnected
            led_state = !led_state;
            gpio_set_level(STATUS_LED_GPIO, led_state);
            vTaskDelay(pdMS_TO_TICKS(LED_BLINK_DISCONNECTED_MS));
        }
    }
}

// ============================================================================
// WEBSOCKET CLIENT MANAGEMENT
// ============================================================================

static void ws_clients_init(void)
{
    ws_clients_mutex = xSemaphoreCreateMutex();
    
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        ws_clients[i].fd = -1;
        ws_clients[i].active = false;
        ws_clients[i].message_queue = xQueueCreate(WS_MESSAGE_QUEUE_SIZE, sizeof(ws_message_t));
    }
    
    ESP_LOGI(TAG, "WebSocket client manager initialized");
}

static int ws_client_add(int fd)
{
    xSemaphoreTake(ws_clients_mutex, portMAX_DELAY);
    
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (!ws_clients[i].active) {
            ws_clients[i].fd = fd;
            ws_clients[i].active = true;
            // Clear any stale messages
            xQueueReset(ws_clients[i].message_queue);
            
            xSemaphoreGive(ws_clients_mutex);
            ESP_LOGI(TAG, "WebSocket client %d connected (fd=%d)", i, fd);
            return i;
        }
    }
    
    xSemaphoreGive(ws_clients_mutex);
    ESP_LOGW(TAG, "No available client slots (fd=%d)", fd);
    return -1;
}

static void ws_client_remove(int fd)
{
    xSemaphoreTake(ws_clients_mutex, portMAX_DELAY);
    
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (ws_clients[i].active && ws_clients[i].fd == fd) {
            ws_clients[i].active = false;
            ws_clients[i].fd = -1;
            xQueueReset(ws_clients[i].message_queue);
            
            ESP_LOGI(TAG, "WebSocket client %d disconnected (fd=%d)", i, fd);
            break;
        }
    }
    
    xSemaphoreGive(ws_clients_mutex);
}

static void ws_broadcast_message(const ws_message_t *msg)
{
    xSemaphoreTake(ws_clients_mutex, portMAX_DELAY);
    
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (ws_clients[i].active) {
            if (xQueueSend(ws_clients[i].message_queue, msg, 0) != pdTRUE) {
                ESP_LOGW(TAG, "Client %d message queue full, dropping message", i);
            }
        }
    }
    
    xSemaphoreGive(ws_clients_mutex);
}

// ============================================================================
// JSON MESSAGE BUILDERS
// ============================================================================

static void build_temperature_message(ws_message_t *msg, const temp_state_t *temps)
{
    msg->type = MSG_TYPE_TEMPERATURE;
    snprintf(msg->json_payload, WS_MAX_PAYLOAD_SIZE,
        "{\"type\":\"temperature\","
        "\"nozzle\":{\"current\":%.1f,\"target\":%.1f},"
        "\"bed\":{\"current\":%.1f,\"target\":%.1f},"
        "\"heatbreak\":{\"current\":%.1f,\"target\":%.1f},"
        "\"chamber\":{\"current\":%.1f}}",
        temps->nozzle_current, temps->nozzle_target,
        temps->bed_current, temps->bed_target,
        temps->heatbreak_current, temps->heatbreak_target,
        temps->chamber_current);
}

static void build_progress_message(ws_message_t *msg, const progress_state_t *progress)
{
    msg->type = MSG_TYPE_PROGRESS;
    snprintf(msg->json_payload, WS_MAX_PAYLOAD_SIZE,
        "{\"type\":\"progress\","
        "\"percent\":%d,"
        "\"timeLeft\":%d,"
        "\"changeTime\":%d}",
        progress->percent,
        progress->time_left_mins,
        progress->change_mins);
}

static void build_position_message(ws_message_t *msg, const position_state_t *pos)
{
    msg->type = MSG_TYPE_POSITION;
    snprintf(msg->json_payload, WS_MAX_PAYLOAD_SIZE,
        "{\"type\":\"position\","
        "\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"e\":%.2f}",
        pos->x, pos->y, pos->z, pos->e);
}

static void build_power_message(ws_message_t *msg, const power_state_t *power)
{
    msg->type = MSG_TYPE_POWER;
    snprintf(msg->json_payload, WS_MAX_PAYLOAD_SIZE,
        "{\"type\":\"power\","
        "\"nozzle\":%d,\"bed\":%d,\"heatbreak\":%d}",
        power->nozzle_pwm,
        power->bed_pwm,
        power->heatbreak_pwm);
}

static void build_log_message(ws_message_t *msg, const char *log_line)
{
    msg->type = MSG_TYPE_LOG;
    
    // Escape quotes and backslashes in log line for JSON
    char escaped[WS_MAX_PAYLOAD_SIZE / 2];
    size_t j = 0;
    for (size_t i = 0; log_line[i] && j < sizeof(escaped) - 2; i++) {
        if (log_line[i] == '"' || log_line[i] == '\\') {
            escaped[j++] = '\\';
        }
        escaped[j++] = log_line[i];
    }
    escaped[j] = '\0';
    
    snprintf(msg->json_payload, WS_MAX_PAYLOAD_SIZE,
        "{\"type\":\"log\",\"message\":\"%s\"}", escaped);
}

static void build_status_message(ws_message_t *msg, bool connected)
{
    msg->type = MSG_TYPE_STATUS;
    snprintf(msg->json_payload, WS_MAX_PAYLOAD_SIZE,
        "{\"type\":\"status\",\"connected\":%s}",
        connected ? "true" : "false");
}

static int ws_log_vprintf(const char *fmt, va_list args)
{
    // Always print to UART first
    int ret = vprintf(fmt, args);
    
    char log_buffer[256];
    int len = vsnprintf(log_buffer, sizeof(log_buffer), fmt, args);
    
    if (len > 0 && len < sizeof(log_buffer)) {
        if (log_buffer[len-1] == '\n') {
            log_buffer[len-1] = '\0';
        }
        
        ws_message_t msg;
        char prefixed_log[300];
        snprintf(prefixed_log, sizeof(prefixed_log), "[ESP] %s", log_buffer);
        build_log_message(&msg, prefixed_log);
        ws_broadcast_message(&msg);
    }
    
    return ret;
}
// ============================================================================
// SERIAL LINE PARSER - THE HEART OF V3!
// ============================================================================

static void parse_and_broadcast_line(const char *line)
{
    ws_message_t msg;
    bool state_changed = false;
    
    // Always send log message
    build_log_message(&msg, line);
    ws_broadcast_message(&msg);
    
    xSemaphoreTake(printer_state_mutex, portMAX_DELAY);
    
    // Temperature parsing - handle A: field (mainboard temp, not displayed)
    if (strstr(line, "T:") && strstr(line, "B:")) {
        const char *t_pos = strstr(line, "T:");
        const char *b_pos = strstr(line, "B:");
        
        float nozzle_cur, nozzle_tgt, bed_cur, bed_tgt;
        
        // Parse T: and B: separately to handle A: field in between
        if (t_pos && sscanf(t_pos, "T:%f/%f", &nozzle_cur, &nozzle_tgt) == 2 &&
            b_pos && sscanf(b_pos, "B:%f/%f", &bed_cur, &bed_tgt) == 2) {
            
            current_temps.nozzle_current = nozzle_cur;
            current_temps.nozzle_target = nozzle_tgt;
            current_temps.bed_current = bed_cur;
            current_temps.bed_target = bed_tgt;
            state_changed = true;
        }
        
        // Heatbreak temperature: X:45.0/45.0
        const char *x_pos = strstr(line, "X:");
        if (x_pos) {
            float hb_cur, hb_tgt;
            if (sscanf(x_pos, "X:%f/%f", &hb_cur, &hb_tgt) == 2) {
                current_temps.heatbreak_current = hb_cur;
                current_temps.heatbreak_target = hb_tgt;
            }
        }
        
        // Chamber temperature: C@:22.5
        const char *c_pos = strstr(line, "C@:");
        if (c_pos) {
            float chamber;
            if (sscanf(c_pos, "C@:%f", &chamber) == 1) {
                current_temps.chamber_current = chamber;
            }
        }
        
        if (state_changed) {
            build_temperature_message(&msg, &current_temps);
            ws_broadcast_message(&msg);
        }
        
        // Power parsing: @:127 B@:64 HBR@:89
        const char *nozzle_pwr = strstr(line, "@:");
        const char *bed_pwr = strstr(line, "B@:");
        const char *hb_pwr = strstr(line, "HBR@:");
        
        bool power_changed = false;
        if (nozzle_pwr && sscanf(nozzle_pwr, "@:%d", &current_power.nozzle_pwm) == 1) {
            power_changed = true;
        }
        if (bed_pwr && sscanf(bed_pwr, "B@:%d", &current_power.bed_pwm) == 1) {
            power_changed = true;
        }
        if (hb_pwr && sscanf(hb_pwr, "HBR@:%d", &current_power.heatbreak_pwm) == 1) {
            power_changed = true;
        }
        
        if (power_changed) {
            build_power_message(&msg, &current_power);
            ws_broadcast_message(&msg);
        }
    }
    
    // Progress parsing: "M73 Progress: 9%;"
    if (strstr(line, "Progress:")) {
        int percent;
        if (sscanf(line, "%*[^P]Progress: %d%%", &percent) == 1) {
            current_progress.percent = percent;
            build_progress_message(&msg, &current_progress);
            ws_broadcast_message(&msg);
        }
    }
    
    // Time left parsing: "Time left: 19m;" or "Time left: 1h 23m;"
    if (strstr(line, "Time left:")) {
        int hours = 0, mins = 0;
        const char *time_str = strstr(line, "Time left:");
        if (time_str) {
            if (sscanf(time_str, "Time left: %dh %dm", &hours, &mins) == 2) {
                current_progress.time_left_mins = hours * 60 + mins;
            } else if (sscanf(time_str, "Time left: %dm", &mins) == 1) {
                current_progress.time_left_mins = mins;
            }
            build_progress_message(&msg, &current_progress);
            ws_broadcast_message(&msg);
        }
    }
    
    // Change time parsing: "Change: 16m;" or "Change: 1h 5m;"
    if (strstr(line, "Change:")) {
        int hours = 0, mins = 0;
        const char *change_str = strstr(line, "Change:");
        if (change_str) {
            if (sscanf(change_str, "Change: %dh %dm", &hours, &mins) == 2) {
                current_progress.change_mins = hours * 60 + mins;
            } else if (sscanf(change_str, "Change: %dm", &mins) == 1) {
                current_progress.change_mins = mins;
            }
            build_progress_message(&msg, &current_progress);
            ws_broadcast_message(&msg);
        }
    }
    
    // Position parsing: X:108.67 Y:90.41 Z:2.20 E:0.00
    if (strstr(line, "X:") && strstr(line, "Y:") && strstr(line, "Z:") && strstr(line, "E:")) {
        // Split at "Count" if present
        char temp_line[SERIAL_LINE_BUFFER_SIZE];
        strncpy(temp_line, line, sizeof(temp_line) - 1);
        temp_line[sizeof(temp_line) - 1] = '\0';
        
        char *count_pos = strstr(temp_line, "Count");
        if (count_pos) *count_pos = '\0';
        
        float x, y, z, e;
        if (sscanf(temp_line, "%*[^X]X:%f Y:%f Z:%f E:%f", &x, &y, &z, &e) == 4) {
            current_position.x = x;
            current_position.y = y;
            current_position.z = z;
            current_position.e = e;
            build_position_message(&msg, &current_position);
            ws_broadcast_message(&msg);
        }
    }
    
    // Print completion detection
    if (strstr(line, "Done printing file")) {
        current_progress.percent = 100;
        current_progress.time_left_mins = 0;
        build_progress_message(&msg, &current_progress);
        ws_broadcast_message(&msg);
    }
    
    xSemaphoreGive(printer_state_mutex);
}

// ============================================================================
// USB CDC COMMUNICATION
// ============================================================================

static bool handle_rx(const uint8_t *data, size_t data_len, void *arg)
{
    // Process incoming serial data byte by byte, building complete lines
    for (size_t i = 0; i < data_len; i++) {
        char c = (char)data[i];
        
        // Handle line endings
        if (c == '\n' || c == '\r') {
            if (serial_line_pos > 0) {
                serial_line_buffer[serial_line_pos] = '\0';
                
                // Parse and broadcast the complete line
                parse_and_broadcast_line(serial_line_buffer);
                
                // Reset buffer for next line
                serial_line_pos = 0;
            }
        } else {
            // Add character to buffer
            if (serial_line_pos < SERIAL_LINE_BUFFER_SIZE - 1) {
                serial_line_buffer[serial_line_pos++] = c;
            } else {
                // Buffer overflow - parse what we have and reset
                serial_line_buffer[SERIAL_LINE_BUFFER_SIZE - 1] = '\0';
                ESP_LOGW(TAG, "Line buffer overflow, forcing parse");
                parse_and_broadcast_line(serial_line_buffer);
                serial_line_pos = 0;
            }
        }
    }
    
    return true;
}

static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
        case CDC_ACM_HOST_ERROR:
            ESP_LOGE(TAG, "CDC-ACM error has occurred, err_no = %d", event->data.error);
            break;
        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGW(TAG, "Printer disconnected");
            xSemaphoreTake(printer_state_mutex, portMAX_DELAY);
            printer_connected = false;
            xSemaphoreGive(printer_state_mutex);
            
            // Broadcast disconnection status
            ws_message_t msg;
            build_status_message(&msg, false);
            ws_broadcast_message(&msg);
            
            xSemaphoreGive(device_disconnected_sem);
            break;
        case CDC_ACM_HOST_SERIAL_STATE:
            ESP_LOGI(TAG, "Serial state notif 0x%04X", event->data.serial_state.val);
            break;
        case CDC_ACM_HOST_NETWORK_CONNECTION:
        default:
            break;
    }
}

static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(TAG, "No more USB clients");
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "All USB devices freed");
        }
    }
}

// ============================================================================
// REMOTE HTML DOWNLOAD (Preserved from V2)
// ============================================================================

typedef struct {
    char *buffer;
    int len;
} download_buffer_t;

esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    download_buffer_t *output = (download_buffer_t *)evt->user_data;
    
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                if (output->buffer == NULL) {
                    int content_length = esp_http_client_get_content_length(evt->client);
                    ESP_LOGI(TAG, "Downloading HTML, size: %d bytes", content_length);
                    
                    if (content_length <= 0 || content_length > 500000) {
                        ESP_LOGE(TAG, "Invalid content length: %d", content_length);
                        return ESP_FAIL;
                    }
                    
                    output->buffer = (char *)malloc(content_length + 1);
                    if (output->buffer == NULL) {
                        ESP_LOGE(TAG, "Failed to allocate memory");
                        return ESP_FAIL;
                    }
                    output->len = 0;
                }
                
                memcpy(output->buffer + output->len, evt->data, evt->data_len);
                output->len += evt->data_len;
                output->buffer[output->len] = '\0';
            }
            break;
            
        default:
            break;
    }
    return ESP_OK;
}

void download_html_from_github(void)
{
    ESP_LOGI(TAG, "Downloading HTML from GitHub...");
    snprintf(last_download_error, sizeof(last_download_error), "Starting download...");
    
    download_buffer_t download = {.buffer = NULL, .len = 0};
    
    esp_http_client_config_t config = {
        .url = REMOTE_HTML_URL,
        .event_handler = http_event_handler,
        .user_data = &download,
        .timeout_ms = 10000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        snprintf(last_download_error, sizeof(last_download_error), 
                 "Failed to initialize HTTP client");
        return;
    }
    
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        
        if (status_code == 200 && download.buffer != NULL && download.len > 0) {
            xSemaphoreTake(html_mutex, portMAX_DELAY);
            if (cached_html != NULL) {
                free(cached_html);
            }
            cached_html = download.buffer;
            cached_html_size = download.len;
            xSemaphoreGive(html_mutex);
            
            snprintf(last_download_error, sizeof(last_download_error), 
                     "Success! Downloaded %d bytes", download.len);
            ESP_LOGI(TAG, "HTML cached successfully (%d bytes)", download.len);
        } else {
            snprintf(last_download_error, sizeof(last_download_error), 
                     "HTTP %d, len=%d", status_code, download.len);
            ESP_LOGE(TAG, "Download failed: HTTP %d", status_code);
            if (download.buffer != NULL) {
                free(download.buffer);
            }
        }
    } else {
        snprintf(last_download_error, sizeof(last_download_error), 
                 "Failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "Download error: %s", esp_err_to_name(err));
        if (download.buffer != NULL) {
            free(download.buffer);
        }
    }
    
    esp_http_client_cleanup(client);
}

// ============================================================================
// WIFI INITIALIZATION (Preserved from V2)
// ============================================================================

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected from WiFi, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization finished. Connecting to %s...", WIFI_SSID);
}

// ============================================================================
// WEBSOCKET MESSAGE SENDER TASK
// ============================================================================

static void ws_sender_task(void *arg)
{
    ws_message_t msg;
    
    while (1) {
        xSemaphoreTake(ws_clients_mutex, portMAX_DELAY);
        
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            if (ws_clients[i].active) {
                // Check if there are messages in this client's queue
                if (xQueueReceive(ws_clients[i].message_queue, &msg, 0) == pdTRUE) {
                    // Send WebSocket frame
                    httpd_ws_frame_t ws_pkt;
                    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
                    ws_pkt.payload = (uint8_t *)msg.json_payload;
                    ws_pkt.len = strlen(msg.json_payload);
                    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
                    
                    esp_err_t ret = httpd_ws_send_frame_async(server, ws_clients[i].fd, &ws_pkt);
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to send to client %d: %s", i, esp_err_to_name(ret));
                        // Client might be dead, will be cleaned up on next connection check
                    }
                }
            }
        }
        
        xSemaphoreGive(ws_clients_mutex);
        
        // Small delay to prevent busy-waiting
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================================
// HTTP/WEBSOCKET HANDLERS
// ============================================================================

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake request");
        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_pkt;
    uint8_t buf[128];
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = buf;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    int fd = httpd_req_to_sockfd(req);
    
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        buf[ws_pkt.len] = '\0';
        ESP_LOGI(TAG, "Received WebSocket packet: %s", buf);
        
        // Check if it's a connection handshake
        if (strcmp((char *)buf, "CONNECT") == 0) {
            int client_id = ws_client_add(fd);
            if (client_id >= 0) {
                // Send current printer state to new client
                ws_message_t msg;
                
                // Send status
                xSemaphoreTake(printer_state_mutex, portMAX_DELAY);
                build_status_message(&msg, printer_connected);
                xQueueSend(ws_clients[client_id].message_queue, &msg, 0);
                
                // Send current temperatures
                build_temperature_message(&msg, &current_temps);
                xQueueSend(ws_clients[client_id].message_queue, &msg, 0);
                
                // Send current progress
                build_progress_message(&msg, &current_progress);
                xQueueSend(ws_clients[client_id].message_queue, &msg, 0);
                
                // Send current position
                build_position_message(&msg, &current_position);
                xQueueSend(ws_clients[client_id].message_queue, &msg, 0);
                
                // Send current power
                build_power_message(&msg, &current_power);
                xQueueSend(ws_clients[client_id].message_queue, &msg, 0);
                
                xSemaphoreGive(printer_state_mutex);
            }
        }
        // Check if it's a G-code command
        else if (strncmp((char *)buf, "GCODE:", 6) == 0) {
            char *cmd = (char *)buf + 6;  // Skip "GCODE:" prefix
            
            if (g_prusa_dev) {
                char cmdline[256];
                snprintf(cmdline, sizeof(cmdline), "%s\n", cmd);
                esp_err_t err = cdc_acm_host_data_tx_blocking(g_prusa_dev, 
                    (uint8_t *)cmdline, strlen(cmdline), USB_TX_TIMEOUT_MS);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to send G-code: %s", esp_err_to_name(err));
                }
            } else {
                ESP_LOGW(TAG, "G-code received but printer not connected");
            }
        }
    } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "WebSocket close frame received");
        ws_client_remove(fd);
    }
    
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    xSemaphoreTake(html_mutex, portMAX_DELAY);
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    
#if ENABLE_REMOTE_HTML
    if (cached_html != NULL && cached_html_size > 0) {
        ESP_LOGI(TAG, "Serving cached remote HTML (%zu bytes)", cached_html_size);
        httpd_resp_send(req, cached_html, cached_html_size);
    } else {
        ESP_LOGW(TAG, "No cached HTML, serving embedded fallback");
        httpd_resp_send(req, (const char *)webpage_start, webpage_end - webpage_start);
    }
#else
    ESP_LOGI(TAG, "Serving embedded HTML");
    httpd_resp_send(req, (const char *)webpage_start, webpage_end - webpage_start);
#endif
    
    xSemaphoreGive(html_mutex);
    return ESP_OK;
}

static esp_err_t refresh_get_handler(httpd_req_t *req)
{
#if ENABLE_REMOTE_HTML
    download_html_from_github();
    
    char response[1024];
    snprintf(response, sizeof(response),
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<meta http-equiv='refresh' content='2;url=/'/>"
        "<style>body{font-family:monospace;background:#0f0f0f;color:#4CAF50;"
        "padding:40px;text-align:center;}</style></head><body>"
        "<h2>HTML Refresh Complete</h2>"
        "<p>Status: %s</p>"
        "<p>Redirecting to monitor...</p>"
        "</body></html>",
        last_download_error);
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, response);
#else
    const char *disabled_msg = 
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<style>body{font-family:monospace;background:#0f0f0f;color:#e0e0e0;"
        "padding:40px;max-width:600px;margin:0 auto;}</style></head><body>"
        "<h2 style='color:#f44336;'>Remote HTML Disabled</h2>"
        "<p>Remote HTML fetching is currently disabled in firmware.</p>"
        "<p>Set ENABLE_REMOTE_HTML to 1 and recompile to enable.</p>"
        "<p><a href='/' style='color:#4CAF50;'>Back to Monitor</a></p>"
        "</body></html>";
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, disabled_msg);
#endif
    return ESP_OK;
}

static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.max_open_sockets = WS_MAX_CLIENTS + 2;  // WS clients + HTTP requests
    
    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Starting HTTP/WebSocket server");
        
        // Root handler
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root_uri);
        
        // Refresh handler
        httpd_uri_t refresh_uri = {
            .uri = "/refresh",
            .method = HTTP_GET,
            .handler = refresh_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &refresh_uri);
        
        // WebSocket handler
        httpd_uri_t ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .user_ctx = NULL,
            .is_websocket = true,
            .handle_ws_control_frames = true
        };
        httpd_register_uri_handler(server, &ws_uri);
        
        ESP_LOGI(TAG, "HTTP/WebSocket server started successfully");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}

// ============================================================================
// MAIN
// ============================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "=== Prusa Core One Monitor V3.0 - WebSocket Edition ===");
    ESP_LOGI(TAG, "Server-side parsing with real-time push updates");
    
    // Create synchronization primitives
    device_disconnected_sem = xSemaphoreCreateBinary();
    html_mutex = xSemaphoreCreateMutex();
    printer_state_mutex = xSemaphoreCreateMutex();
    assert(device_disconnected_sem && html_mutex && printer_state_mutex);
    
    // Initialize WebSocket client manager
    ws_clients_init();
    
    // Initialize USB Host
    ESP_LOGI(TAG, "Initializing USB Host");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, USB_HOST_TASK_PRIORITY, NULL);
    
    // Install CDC-ACM driver
    ESP_LOGI(TAG, "Installing CDC-ACM driver");
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));
    
    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 1000,
        .out_buffer_size = 512,
        .in_buffer_size = 8192,
        .user_arg = NULL,
        .event_cb = handle_event,
        .data_cb = handle_rx
    };
    
    // Initialize NVS and WiFi
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();
    
#if ENABLE_REMOTE_HTML
    // Wait for WiFi connection
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    // Download HTML from GitHub
    download_html_from_github();
#else
    ESP_LOGI(TAG, "Remote HTML fetching disabled - using embedded HTML only");
#endif
    
    // Initialize mDNS
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set("coreone");
    mdns_instance_name_set("Prusa Core One Monitor V3.0");
    ESP_LOGI(TAG, "mDNS started: http://coreone.local/");
    
    // Start web server
    start_webserver();
    
    // Start WebSocket message sender task
    xTaskCreate(ws_sender_task, "ws_sender", 4096, NULL, 5, NULL);
    
    // Start LED task
    xTaskCreate(led_task, "led_task", 2048, NULL, 3, &led_task_handle);
    
    ESP_LOGI(TAG, "=== System Ready ===");
    ESP_LOGI(TAG, "Access web interface at:");
    ESP_LOGI(TAG, "  - http://coreone.local/");
    ESP_LOGI(TAG, "  - WebSocket: ws://coreone.local/ws");
    ESP_LOGI(TAG, "  - Manual HTML refresh: http://coreone.local/refresh");
    

    // NOW install log handler after everything is initialized
    esp_log_set_vprintf(ws_log_vprintf);
    ESP_LOGI(TAG, "WebSocket logging active");

    // Main USB connection loop
    while (true) {
        esp_err_t err = cdc_acm_host_open(PRUSA_USB_VID, PRUSA_USB_PID, 0, 
                                          &dev_config, &g_prusa_dev);
        if (err != ESP_OK) {
            if (err == ESP_ERR_NOT_FOUND) {
                ESP_LOGD(TAG, "Printer not found, retrying...");
            } else {
                ESP_LOGW(TAG, "Failed to open printer: %s", esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        
        ESP_LOGI(TAG, "Printer connected!");
        cdc_acm_host_desc_print(g_prusa_dev);
        vTaskDelay(pdMS_TO_TICKS(200));
        
        // Update connection state
        xSemaphoreTake(printer_state_mutex, portMAX_DELAY);
        printer_connected = true;
        xSemaphoreGive(printer_state_mutex);
        
        // Broadcast connection status
        ws_message_t msg;
        build_status_message(&msg, true);
        ws_broadcast_message(&msg);
        
        // Configure serial port
        cdc_acm_line_coding_t line_coding = {
            .dwDTERate = 115200,
            .bDataBits = 8,
            .bParityType = 0,
            .bCharFormat = 0
        };
        ESP_ERROR_CHECK(cdc_acm_host_line_coding_set(g_prusa_dev, &line_coding));
        ESP_ERROR_CHECK(cdc_acm_host_set_control_line_state(g_prusa_dev, true, false));
        
        // Send initial commands (only once)
        if (!initial_chirp_sent) {
            ESP_LOGI(TAG, "Sending initial beep");
            ESP_ERROR_CHECK(cdc_acm_host_data_tx_blocking(g_prusa_dev,
                (const uint8_t *)INITIAL_BEEP_COMMAND,
                strlen(INITIAL_BEEP_COMMAND), USB_TX_TIMEOUT_MS));
            
            // Enable temperature reporting and progress updates
            const char *init_cmds = "M155 S2\nM73\n";
            ESP_LOGI(TAG, "Enabling temperature reporting");
            ESP_ERROR_CHECK(cdc_acm_host_data_tx_blocking(g_prusa_dev,
                (const uint8_t *)init_cmds, strlen(init_cmds), USB_TX_TIMEOUT_MS));
            
            initial_chirp_sent = true;
        }
        
        // Wait for disconnect
        xSemaphoreTake(device_disconnected_sem, portMAX_DELAY);
        ESP_LOGI(TAG, "Printer disconnected, waiting for reconnection...");
    }
}
