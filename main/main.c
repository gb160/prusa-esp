#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "mdns.h"

extern const uint8_t webpage_start[] asm("_binary_webpage_html_start");
extern const uint8_t webpage_end[] asm("_binary_webpage_html_end");

// -----------------------------
// Configs
// -----------------------------
#define EXAMPLE_USB_HOST_PRIORITY   (20)
#define EXAMPLE_USB_DEVICE_VID      (0x2C99) // Prusa Core One VID
#define EXAMPLE_USB_DEVICE_PID      (0x001F) // Prusa Core One PID
#define EXAMPLE_TX_STRING           ("M300 S2000 P50\n")
#define EXAMPLE_TX_TIMEOUT_MS       (1000)

#define WIFI_SSID      "BT-WXF9FJ"
#define WIFI_PASS      "QFLQCPDLWF"

static const char *TAG = "USB-CDC";
static SemaphoreHandle_t device_disconnected_sem;
static SemaphoreHandle_t log_mutex;
static cdc_acm_dev_hdl_t g_prusa_dev = NULL;
static bool initial_chirp_sent = false;

// -----------------------------
// Log ring buffer
// -----------------------------
#define LOG_BUFFER_SIZE 8192
static char log_buffer[LOG_BUFFER_SIZE];
static size_t log_start = 0;  // start of valid data
static size_t log_end = 0;    // end of valid data

// -----------------------------
// Temperatures
// -----------------------------
typedef struct { int cur; int target; } temp_t;
static temp_t nozzle_temp = {0,0};
static temp_t bed_temp = {0,0};
static temp_t heatbreak_temp = {0,0};
static temp_t chamber_temp = {0,0};
static int nozzle_power = 0;
static int bed_power = 0;
static int heatbreak_power = 0;

// -----------------------------
// Progress readings
// -----------------------------
static int prog_percent = 0;
static int prog_time = 0;
static int prog_change = 0;

// -----------------------------
// USB CDC callbacks
// -----------------------------
static bool handle_rx(const uint8_t *data, size_t data_len, void *arg)
{
    xSemaphoreTake(log_mutex, portMAX_DELAY);

    for(size_t i=0;i<data_len;i++){
        log_buffer[log_end] = data[i];
        log_end = (log_end + 1) % LOG_BUFFER_SIZE;
        if(log_end == log_start){ // buffer full, advance start
            log_start = (log_start + 1) % LOG_BUFFER_SIZE;
        }
    }

    // parse for temperature and progress lines
    char line[256];
    static size_t parser_pos = 0;
    while(parser_pos != log_end){
        size_t line_len = 0;
        size_t i = parser_pos;
        while(i != log_end && log_buffer[i] != '\n' && log_buffer[i] != '\r' && line_len<sizeof(line)-1){
            line[line_len++] = log_buffer[i];
            i = (i+1) % LOG_BUFFER_SIZE;
        }
        line[line_len] = 0;
        parser_pos = (i != log_end) ? (i+1)%LOG_BUFFER_SIZE : i;

        // parse temperatures
        float cur,target; const char *p;
        p = strstr(line,"T:"); if(p && sscanf(p,"T:%f/%f",&cur,&target)==2){ nozzle_temp.cur=(int)(cur+0.5); nozzle_temp.target=(int)(target+0.5);}
        p = strstr(line," B:"); if(p && sscanf(p," B:%f/%f",&cur,&target)==2){ bed_temp.cur=(int)(cur+0.5); bed_temp.target=(int)(target+0.5);}
        p = strstr(line," X:"); if(p && sscanf(p," X:%f/%f",&cur,&target)==2){ heatbreak_temp.cur=(int)(cur+0.5); heatbreak_temp.target=(int)(target+0.5);}
        p = strstr(line," C@:"); if(p && sscanf(p," C@:%f",&cur)==1){ chamber_temp.cur=(int)(cur+0.5);}

        // parse new progress format: P:10 R:75 C:NA
        int p_val=0, r_val=0;
        char cbuf[8];

        p = strstr(line, "P:"); // look for P: anywhere in the line
        if(p && sscanf(p, "P:%d R:%d C:%7s", &p_val, &r_val, cbuf) == 3){
            prog_percent = p_val;
            prog_time    = r_val;
            if(strcmp(cbuf,"NA")==0)
                prog_change = -1;
            else
                prog_change = atoi(cbuf);
        }

        // optional: same logic as before for zero progress
        if(prog_percent == 0 && prog_change == 0){
            prog_time = 0;
        }
    }

    xSemaphoreGive(log_mutex);
    return true;
}


static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch(event->type){
        case CDC_ACM_HOST_ERROR:
            ESP_LOGE(TAG,"CDC error: %d",event->data.error);
            break;
        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGI(TAG,"Device disconnected");
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
    while(1){
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if(event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS){
            ESP_ERROR_CHECK(usb_host_device_free_all());
        }
    }
}

// -----------------------------
// Temperature & progress parser
// -----------------------------
static void parse_temp_line(const char *line)
{
    float cur, target;
    const char *p;

    // Nozzle
    p = strstr(line,"T:");
    if(p && sscanf(p,"T:%f/%f",&cur,&target)==2){
        xSemaphoreTake(log_mutex, portMAX_DELAY);
        nozzle_temp.cur = (int)(cur+0.5);
        nozzle_temp.target = (int)(target+0.5);
        xSemaphoreGive(log_mutex);
    }

    // Bed
    p = strstr(line," B:");
    if(p && sscanf(p," B:%f/%f",&cur,&target)==2){
        xSemaphoreTake(log_mutex, portMAX_DELAY);
        bed_temp.cur = (int)(cur+0.5);
        bed_temp.target = (int)(target+0.5);
        xSemaphoreGive(log_mutex);
    }

    // Heatbreak
    p = strstr(line," X:");
    if(p && sscanf(p," X:%f/%f",&cur,&target)==2){
        xSemaphoreTake(log_mutex, portMAX_DELAY);
        heatbreak_temp.cur = (int)(cur+0.5);
        heatbreak_temp.target = (int)(target+0.5);
        xSemaphoreGive(log_mutex);
    }

    // Chamber
    p = strstr(line," C@:");
    if(p && sscanf(p," C@:%f",&cur)==1){
        xSemaphoreTake(log_mutex, portMAX_DELAY);
        chamber_temp.cur = (int)(cur+0.5);
        xSemaphoreGive(log_mutex);
    }



    // Nozzle power
    p = strstr(line," @:");
    if(p && sscanf(p," @:%f",&cur)==1) {
        xSemaphoreTake(log_mutex, portMAX_DELAY);
        nozzle_power = (int)(cur+0.5);
        xSemaphoreGive(log_mutex);
    }

    // Bed power
    p = strstr(line," B@:");
    if(p && sscanf(p," B@:%f",&cur)==1) {
        xSemaphoreTake(log_mutex, portMAX_DELAY);
        bed_power = (int)(cur+0.5);
        xSemaphoreGive(log_mutex);
    }

    // Heatbreak power
    p = strstr(line," HBR@:");
    if(p && sscanf(p," HBR@:%f",&cur)==1) {
        xSemaphoreTake(log_mutex, portMAX_DELAY);
        heatbreak_power = (int)(cur+0.5);
        xSemaphoreGive(log_mutex);
    }

}

static void temp_parser_task(void *arg)
{
    static size_t parser_pos = 0;
    char line[256];

    while(1) {
        xSemaphoreTake(log_mutex, portMAX_DELAY);
        size_t start = parser_pos;
        size_t end = log_end;
        xSemaphoreGive(log_mutex);

        while(start != end) {
            line[0]=0;
            size_t line_len=0;

            xSemaphoreTake(log_mutex, portMAX_DELAY);
            size_t i = start;
            while(i != end && log_buffer[i] != '\n' && log_buffer[i] != '\r' && line_len<sizeof(line)-1){
                line[line_len++] = log_buffer[i];
                i = (i+1) % LOG_BUFFER_SIZE;
            }
            line[line_len]=0;
            start = (i != end) ? (i+1)%LOG_BUFFER_SIZE : i;
            xSemaphoreGive(log_mutex);

            if(line_len>0){
                parse_temp_line(line);
            }
        }

        parser_pos = start;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// -----------------------------
// Wi-Fi
// -----------------------------
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START){
        esp_wifi_connect();
    } else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED){
        esp_wifi_connect();
    }
}

void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

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

// -----------------------------
// Web server HTML & HTTP handlers
// -----------------------------
static httpd_handle_t server = NULL;




// -----------------------------
// HTTP handlers
// -----------------------------
static esp_err_t root_get_handler(httpd_req_t *req) {
    const size_t webpage_size = (webpage_end - webpage_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)webpage_start, webpage_size);
    return ESP_OK;
}

static esp_err_t send_get_handler(httpd_req_t *req) {
    char query[256];
    if(httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[256];
        if(httpd_query_key_value(query, "cmd", param, sizeof(param)) == ESP_OK){
            if(g_prusa_dev){
                char cmdline[512];
                snprintf(cmdline,sizeof(cmdline),"%s\n",param);
                cdc_acm_host_data_tx_blocking(g_prusa_dev,(uint8_t*)cmdline,strlen(cmdline),1000);
            }
        }
    }
    httpd_resp_sendstr(req,"OK");
    return ESP_OK;
}

static esp_err_t log_get_handler(httpd_req_t *req){
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    if(log_start != log_end){
        if(log_end > log_start){
            httpd_resp_send(req, log_buffer + log_start, log_end - log_start);
        } else {
            httpd_resp_send(req, log_buffer + log_start, LOG_BUFFER_SIZE - log_start);
            if(log_end > 0)
                httpd_resp_send(req, log_buffer, log_end);
        }
        log_start = log_end;
    } else {
        httpd_resp_sendstr(req,"");
    }
    xSemaphoreGive(log_mutex);
    return ESP_OK;
}

static esp_err_t clear_get_handler(httpd_req_t *req){
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    log_start = 0;
    log_end = 0;
    xSemaphoreGive(log_mutex);
    httpd_resp_sendstr(req,"OK");
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req){
    const char* status = g_prusa_dev ? "1" : "0";
    httpd_resp_sendstr(req,status);
    return ESP_OK;
}

static esp_err_t temps_get_handler(httpd_req_t *req){
    char buf[300];
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    
    if(prog_change == -1) {
        // Change is NA - send as string "NA"
        snprintf(buf,sizeof(buf),
                "{\"nozzle\":{\"cur\":%d,\"target\":%d},"
                "\"bed\":{\"cur\":%d,\"target\":%d},"
                "\"heatbreak\":{\"cur\":%d,\"target\":%d},"
                "\"board\":{\"cur\":%d},"
                "\"chamber\":{\"cur\":%d},"
                "\"nozzle_power\":%d,"
                "\"bed_power\":%d,"
                "\"heatbreak_power\":%d,"
                "\"progress\":%d,"
                "\"time\":%d,"
                "\"change\":\"NA\"}",
                nozzle_temp.cur,nozzle_temp.target,
                bed_temp.cur,bed_temp.target,
                heatbreak_temp.cur,heatbreak_temp.target,
                0,
                chamber_temp.cur,
                nozzle_power,
                bed_power,
                heatbreak_power,
                prog_percent,prog_time);
    } else {
        snprintf(buf,sizeof(buf),
                "{\"nozzle\":{\"cur\":%d,\"target\":%d},"
                "\"bed\":{\"cur\":%d,\"target\":%d},"
                "\"heatbreak\":{\"cur\":%d,\"target\":%d},"
                "\"board\":{\"cur\":%d},"
                "\"chamber\":{\"cur\":%d},"
                "\"nozzle_power\":%d,"
                "\"bed_power\":%d,"
                "\"heatbreak_power\":%d,"
                "\"progress\":%d,"
                "\"time\":%d,"
                "\"change\":%d}",
                nozzle_temp.cur,nozzle_temp.target,
                bed_temp.cur,bed_temp.target,
                heatbreak_temp.cur,heatbreak_temp.target,
                0,
                chamber_temp.cur,
                nozzle_power,
                bed_power,
                heatbreak_power,
                prog_percent,prog_time,prog_change);
    }

    xSemaphoreGive(log_mutex);
    httpd_resp_set_type(req,"application/json");
    httpd_resp_sendstr(req,buf);
    return ESP_OK;
}

// -----------------------------
// Start webserver
// -----------------------------
static void start_webserver(void){
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    if(httpd_start(&server,&config)==ESP_OK){
        httpd_uri_t root_uri = {"/",HTTP_GET,root_get_handler,NULL};
        httpd_register_uri_handler(server,&root_uri);
        httpd_uri_t send_uri = {"/send",HTTP_GET,send_get_handler,NULL};
        httpd_register_uri_handler(server,&send_uri);
        httpd_uri_t log_uri = {"/log",HTTP_GET,log_get_handler,NULL};
        httpd_register_uri_handler(server,&log_uri);
        httpd_uri_t clear_uri = {"/clear",HTTP_GET,clear_get_handler,NULL};
        httpd_register_uri_handler(server,&clear_uri);
        httpd_uri_t status_uri = {"/status",HTTP_GET,status_get_handler,NULL};
        httpd_register_uri_handler(server,&status_uri);
        httpd_uri_t temps_uri = {"/temps",HTTP_GET,temps_get_handler,NULL};
        httpd_register_uri_handler(server,&temps_uri);
    }
}

// -----------------------------
// Main
// -----------------------------
void app_main(void){
    device_disconnected_sem = xSemaphoreCreateBinary();
    log_mutex = xSemaphoreCreateMutex();
    assert(device_disconnected_sem && log_mutex);

    ESP_LOGI(TAG,"Installing USB Host");
    const usb_host_config_t host_config = {.skip_phy_setup=false,.intr_flags=ESP_INTR_FLAG_LEVEL1};
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskCreate(usb_lib_task,"usb_lib",4096,NULL,EXAMPLE_USB_HOST_PRIORITY,NULL);
    ESP_LOGI(TAG,"Installing CDC-ACM driver");
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));

    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms=1000,
        .out_buffer_size=512,
        .in_buffer_size=8192,
        .user_arg=NULL,
        .event_cb=handle_event,
        .data_cb=handle_rx
    };

    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set("coreone");
    mdns_instance_name_set("Prusa ESP32 Controller");
    ESP_LOGI(TAG,"Access web UI at http://coreone.local/");

    start_webserver();

    xTaskCreate(temp_parser_task,"temp_parser",4096,NULL,5,NULL);

    while(true){
        esp_err_t err = cdc_acm_host_open(EXAMPLE_USB_DEVICE_VID,EXAMPLE_USB_DEVICE_PID,0,&dev_config,&g_prusa_dev);
        if(err!=ESP_OK){
            ESP_LOGW(TAG,"No USB CDC device found. Retrying...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        cdc_acm_host_desc_print(g_prusa_dev);
        vTaskDelay(pdMS_TO_TICKS(200));

        cdc_acm_line_coding_t line_coding = {.dwDTERate=115200,.bDataBits=8,.bParityType=0,.bCharFormat=0};
        ESP_ERROR_CHECK(cdc_acm_host_line_coding_set(g_prusa_dev,&line_coding));
        ESP_ERROR_CHECK(cdc_acm_host_set_control_line_state(g_prusa_dev,true,false));

        // Initial chirp and M155 S1 for temperatures
        if(!initial_chirp_sent){
            ESP_LOGI(TAG,"Sending initial chirp: %s",EXAMPLE_TX_STRING);
            ESP_ERROR_CHECK(cdc_acm_host_data_tx_blocking(g_prusa_dev,(const uint8_t*)EXAMPLE_TX_STRING,strlen(EXAMPLE_TX_STRING),EXAMPLE_TX_TIMEOUT_MS));
            const char *m155 = "M155 S2\nM73\n";
            ESP_ERROR_CHECK(cdc_acm_host_data_tx_blocking(g_prusa_dev,(const uint8_t*)m155,strlen(m155),EXAMPLE_TX_TIMEOUT_MS));
            initial_chirp_sent=true;
        }

        xSemaphoreTake(device_disconnected_sem,portMAX_DELAY);
    }
}
