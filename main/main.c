/*
 * Prusa Core One ESP32 Monitor V2.0
 * 
 * This firmware connects an ESP32 to a Prusa Core One 3D printer via USB CDC
 * and provides a web interface for monitoring and control.
 * 
 * V2.0 Changes:
 * - ALL parsing moved to client-side JavaScript
 * - ESP32 only buffers raw serial data
 * - Reduced CPU and memory usage
 * - Simpler, cleaner code
 * 
 * Main Features:
 * - USB CDC communication with printer
 * - Remote HTML loading from GitHub (with embedded fallback)
 * - Raw serial data streaming to browser
 * - G-code command sending via web interface
 * - WiFi connectivity with mDNS (coreone.local)
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
#define WIFI_SSID                   ""
#define WIFI_PASS                   ""

// Remote HTML configuration
#define ENABLE_REMOTE_HTML          (1)  // Set to 0 to disable GitHub fetching and use only embedded HTML
#define REMOTE_HTML_URL             "https://raw.githubusercontent.com/gb160/prusa-esp/main/main/webpage.html"

// Ring buffer for printer log data
#define LOG_BUFFER_SIZE             (8192)

static const char *TAG = "PRUSA-MONITOR-V2";

// ============================================================================
// GLOBAL STATE
// ============================================================================

// Synchronization primitives
static SemaphoreHandle_t device_disconnected_sem;
static SemaphoreHandle_t log_mutex;
static SemaphoreHandle_t html_mutex;

// USB device handle
static cdc_acm_dev_hdl_t g_prusa_dev = NULL;
static bool initial_chirp_sent = false;

// Ring buffer for printer console output - NOW THE ONLY DATA STORE
static char log_buffer[LOG_BUFFER_SIZE];
static size_t log_start = 0;
static size_t log_end = 0;

// Web server handle
static httpd_handle_t server = NULL;

// Embedded webpage (fallback)
extern const uint8_t webpage_start[] asm("_binary_webpage_html_start");
extern const uint8_t webpage_end[] asm("_binary_webpage_html_end");

// Remote HTML cache
static char *cached_html = NULL;
static size_t cached_html_size = 0;
static char last_download_error[256] = "Not attempted yet";

// ============================================================================
// REMOTE HTML DOWNLOAD
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
// USB CDC COMMUNICATION - SIMPLIFIED!
// ============================================================================

/**
 * USB RX Handler - V2.0 SIMPLIFIED VERSION
 * Just buffers raw data - NO PARSING!
 * All parsing now happens in browser JavaScript
 */
static bool handle_rx(const uint8_t *data, size_t data_len, void *arg)
{
    xSemaphoreTake(log_mutex, portMAX_DELAY);

    // Simply buffer the raw data - that's it!
    for (size_t i = 0; i < data_len; i++) {
        log_buffer[log_end] = data[i];
        log_end = (log_end + 1) % LOG_BUFFER_SIZE;
        if (log_end == log_start) {
            // Buffer full, advance start (oldest data gets overwritten)
            log_start = (log_start + 1) % LOG_BUFFER_SIZE;
        }
    }

    xSemaphoreGive(log_mutex);
    return true;
}

static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
        case CDC_ACM_HOST_ERROR:
            ESP_LOGE(TAG, "CDC error: %d", event->data.error);
            break;
        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGI(TAG, "Printer disconnected");
            ESP_ERROR_CHECK(cdc_acm_host_close(event->data.cdc_hdl));
            g_prusa_dev = NULL;
            initial_chirp_sent = false;
            xSemaphoreGive(device_disconnected_sem);
            break;
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
            ESP_ERROR_CHECK(usb_host_device_free_all());
        }
    }
}

// ============================================================================
// WIFI
// ============================================================================

static void wifi_event_handler(void *arg, esp_event_base_t event_base, 
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ============================================================================
// HTTP HANDLERS - SIMPLIFIED!
// ============================================================================

static esp_err_t root_get_handler(httpd_req_t *req)
{
#if ENABLE_REMOTE_HTML
    xSemaphoreTake(html_mutex, portMAX_DELAY);
    
    if (cached_html != NULL && cached_html_size > 0) {
        // Serve downloaded HTML from GitHub
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, cached_html, cached_html_size);
        xSemaphoreGive(html_mutex);
        ESP_LOGI(TAG, "Served GitHub HTML (%d bytes)", cached_html_size);
    } else {
        // Fallback to embedded HTML
        xSemaphoreGive(html_mutex);
        const size_t webpage_size = (webpage_end - webpage_start);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, (const char *)webpage_start, webpage_size);
        ESP_LOGW(TAG, "Served embedded HTML fallback (%d bytes)", webpage_size);
    }
#else
    // Remote HTML disabled - always use embedded
    const size_t webpage_size = (webpage_end - webpage_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)webpage_start, webpage_size);
    ESP_LOGI(TAG, "Served embedded HTML (%d bytes) [Remote fetch disabled]", webpage_size);
#endif
    
    return ESP_OK;
}

static esp_err_t refresh_get_handler(httpd_req_t *req)
{
#if ENABLE_REMOTE_HTML
    ESP_LOGI(TAG, "Manual refresh requested");
    download_html_from_github();
    
    xSemaphoreTake(html_mutex, portMAX_DELAY);
    int is_cached = (cached_html != NULL && cached_html_size > 0);
    int size = cached_html_size;
    xSemaphoreGive(html_mutex);
    
    char buf[768];  // Increased from 512 to 768 bytes
    if (is_cached) {
        snprintf(buf, sizeof(buf),
            "<html><body style='background:#0f0f0f;color:#e0e0e0;padding:40px;font-family:sans-serif;'>"
            "<h1 style='color:#4CAF50;'>✓ Success!</h1>"
            "<p>Downloaded %d bytes from GitHub</p>"
            "<p>The new version will be used on next page load.</p>"
            "<p><a href='/' style='color:#4CAF50;text-decoration:none;'>← Back to Monitor</a></p>"
            "</body></html>",
            size);
    } else {
        snprintf(buf, sizeof(buf),
            "<html><body style='background:#0f0f0f;color:#e0e0e0;padding:40px;font-family:sans-serif;'>"
            "<h1 style='color:#f44336;'>✗ Failed!</h1>"
            "<p><strong>Error:</strong> %s</p>"
            "<p>Using embedded HTML fallback.</p>"
            "<p><a href='/' style='color:#4CAF50;text-decoration:none;'>← Back to Monitor</a></p>"
            "</body></html>",
            last_download_error);
    }
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, buf);
#else
    // Remote HTML disabled
    const char *disabled_msg = 
        "<html><body style='background:#0f0f0f;color:#e0e0e0;padding:40px;font-family:sans-serif;'>"
        "<h1 style='color:#FFA500;'>⚠ Remote HTML Disabled</h1>"
        "<p>Remote HTML fetching is disabled in firmware configuration.</p>"
        "<p>To enable: Set <code>ENABLE_REMOTE_HTML</code> to 1 and recompile.</p>"
        "<p><a href='/' style='color:#4CAF50;text-decoration:none;'>← Back to Monitor</a></p>"
        "</body></html>";
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, disabled_msg);
    ESP_LOGI(TAG, "Refresh endpoint called but remote HTML is disabled");
#endif
    return ESP_OK;
}

static esp_err_t send_get_handler(httpd_req_t *req)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[256];
        if (httpd_query_key_value(query, "cmd", param, sizeof(param)) == ESP_OK) {
            if (g_prusa_dev) {
                char cmdline[512];
                snprintf(cmdline, sizeof(cmdline), "%s\n", param);
                esp_err_t err = cdc_acm_host_data_tx_blocking(g_prusa_dev, 
                    (uint8_t *)cmdline, strlen(cmdline), USB_TX_TIMEOUT_MS);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to send command: %s", esp_err_to_name(err));
                }
            } else {
                ESP_LOGW(TAG, "Command received but printer not connected");
            }
        }
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/**
 * Main log endpoint - serves raw serial data
 * Browser does all the parsing!
 */
static esp_err_t log_get_handler(httpd_req_t *req)
{
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    
    if (log_start != log_end) {
        if (log_end > log_start) {
            // Data is contiguous
            httpd_resp_send(req, log_buffer + log_start, log_end - log_start);
        } else {
            // Data wraps around
            httpd_resp_send(req, log_buffer + log_start, LOG_BUFFER_SIZE - log_start);
            if (log_end > 0) {
                httpd_resp_send(req, log_buffer, log_end);
            }
        }
        // Mark all data as consumed
        log_start = log_end;
    } else {
        httpd_resp_sendstr(req, "");
    }
    
    xSemaphoreGive(log_mutex);
    return ESP_OK;
}

static esp_err_t clear_get_handler(httpd_req_t *req)
{
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    log_start = 0;
    log_end = 0;
    xSemaphoreGive(log_mutex);
    
    ESP_LOGI(TAG, "Log buffer cleared");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    const char *status = g_prusa_dev ? "1" : "0";
    httpd_resp_sendstr(req, status);
    return ESP_OK;
}

static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;  // Increase stack for HTTPS client
    
    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Starting web server");
        
        httpd_uri_t root_uri = {"/", HTTP_GET, root_get_handler, NULL};
        httpd_register_uri_handler(server, &root_uri);
        
        httpd_uri_t refresh_uri = {"/refresh", HTTP_GET, refresh_get_handler, NULL};
        httpd_register_uri_handler(server, &refresh_uri);
        
        httpd_uri_t send_uri = {"/send", HTTP_GET, send_get_handler, NULL};
        httpd_register_uri_handler(server, &send_uri);
        
        httpd_uri_t log_uri = {"/log", HTTP_GET, log_get_handler, NULL};
        httpd_register_uri_handler(server, &log_uri);
        
        httpd_uri_t clear_uri = {"/clear", HTTP_GET, clear_get_handler, NULL};
        httpd_register_uri_handler(server, &clear_uri);
        
        httpd_uri_t status_uri = {"/status", HTTP_GET, status_get_handler, NULL};
        httpd_register_uri_handler(server, &status_uri);
        
        ESP_LOGI(TAG, "Web server started successfully");
    } else {
        ESP_LOGE(TAG, "Failed to start web server");
    }
}

// ============================================================================
// MAIN
// ============================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "=== Prusa Core One Monitor V2.0 ===");
    ESP_LOGI(TAG, "Client-side parsing enabled");
    
    // Create synchronization primitives
    device_disconnected_sem = xSemaphoreCreateBinary();
    log_mutex = xSemaphoreCreateMutex();
    html_mutex = xSemaphoreCreateMutex();
    assert(device_disconnected_sem && log_mutex && html_mutex);
    
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
    mdns_instance_name_set("Prusa Core One Monitor V2.0");
    ESP_LOGI(TAG, "mDNS started: http://coreone.local/");
    
    // Start web server
    start_webserver();
    
    ESP_LOGI(TAG, "=== System Ready ===");
    ESP_LOGI(TAG, "Access web interface at:");
    ESP_LOGI(TAG, "  - http://coreone.local/");
    ESP_LOGI(TAG, "  - Manual refresh: http://coreone.local/refresh");
    
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