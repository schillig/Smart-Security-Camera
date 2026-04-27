#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_camera.h"
#include "esp_http_server.h" 
#include "led_strip.h"
#include "lwip/ip_addr.h" 

static const char *TAG = "SEC_CAM";

#define WIFI_SSID      "WIN_fb71"
#define WIFI_PASS      ""***""
#define BLINK_GPIO     48

#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5
#define CAM_PIN_D7      16
#define CAM_PIN_D6      17
#define CAM_PIN_D5      18
#define CAM_PIN_D4      12
#define CAM_PIN_D3      10
#define CAM_PIN_D2      8
#define CAM_PIN_D1      9
#define CAM_PIN_D0      11
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK    13

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static led_strip_handle_t led_strip;

static void configure_led(void) {
    led_strip_config_t strip_config = { .strip_gpio_num = BLINK_GPIO, .max_leds = 1 };
    led_strip_rmt_config_t rmt_config = { .resolution_hz = 10 * 1000 * 1000, .flags.with_dma = false };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_set_pixel(led_strip, 0, 20, 0, 0); 
    led_strip_refresh(led_strip);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        led_strip_set_pixel(led_strip, 0, 20, 0, 0); 
        led_strip_refresh(led_strip);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        led_strip_set_pixel(led_strip, 0, 15, 15, 15); 
        led_strip_refresh(led_strip);
    }
}

static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_dhcpc_stop(netif); 
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 50, 152);      
    IP4_ADDR(&ip_info.gw, 192, 168, 50, 1);         
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);   
    esp_netif_set_ip_info(netif, &ip_info);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    wifi_config_t wifi_config = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static esp_err_t init_camera(void) {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = CAM_PIN_D0; config.pin_d1 = CAM_PIN_D1;
    config.pin_d2 = CAM_PIN_D2; config.pin_d3 = CAM_PIN_D3;
    config.pin_d4 = CAM_PIN_D4; config.pin_d5 = CAM_PIN_D5;
    config.pin_d6 = CAM_PIN_D6; config.pin_d7 = CAM_PIN_D7;
    config.pin_xclk = CAM_PIN_XCLK; config.pin_pclk = CAM_PIN_PCLK;
    config.pin_vsync = CAM_PIN_VSYNC; config.pin_href = CAM_PIN_HREF;
    config.pin_sccb_sda = CAM_PIN_SIOD; config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_pwdn = CAM_PIN_PWDN; config.pin_reset = CAM_PIN_RESET;
    config.xclk_freq_hz = 10000000;
    config.frame_size = FRAMESIZE_VGA;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 15;
    config.fb_count = 2; 
    return esp_camera_init(&config);
}

// -------------------------------------------------------------
// HTTP API: Endpoint 1 (/stream)
// -------------------------------------------------------------
static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[64];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK) return res;

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while(true) {
        fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        
        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, fb->len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        esp_camera_fb_return(fb);
        if(res != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return res;
}

// -------------------------------------------------------------
// HTTP API: Endpoint 2 (/control)
// -------------------------------------------------------------
static esp_err_t cmd_handler(httpd_req_t *req) {
    char* buf;
    size_t buf_len;
    char variable[32] = {0,};
    char value[32] = {0,};

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK ||
                httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK) {
                free(buf);
                httpd_resp_send_404(req);
                return ESP_FAIL;
            }
        } else {
            free(buf);
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
        free(buf);
    } else {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    int val = atoi(value);
    sensor_t * s = esp_camera_sensor_get();
    int res = 0;

    if(!strcmp(variable, "framesize")) {
        if(s->pixformat == PIXFORMAT_JPEG) res = s->set_framesize(s, (framesize_t)val);
    }
    else if(!strcmp(variable, "quality")) res = s->set_quality(s, val);
    else if(!strcmp(variable, "contrast")) res = s->set_contrast(s, val);
    else if(!strcmp(variable, "brightness")) res = s->set_brightness(s, val);
    else if(!strcmp(variable, "saturation")) res = s->set_saturation(s, val);
    else if(!strcmp(variable, "awb")) res = s->set_whitebal(s, val);
    else if(!strcmp(variable, "aec")) res = s->set_exposure_ctrl(s, val);
    else if(!strcmp(variable, "vflip")) res = s->set_vflip(s, val);
    else if(!strcmp(variable, "hmirror")) res = s->set_hmirror(s, val);
    else { res = -1; }

    if(res) { return httpd_resp_send_500(req); }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// -------------------------------------------------------------
// Boot Sequence
// -------------------------------------------------------------
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    configure_led();
    wifi_init_sta();

    if(init_camera() != ESP_OK) {
        ESP_LOGE(TAG, "Camera failed to initialize. Halting.");
        return;
    }

    // Default Startup Tuning
    sensor_t * s = esp_camera_sensor_get();
    s->set_whitebal(s, 1);       
    s->set_awb_gain(s, 1);       
    s->set_exposure_ctrl(s, 1);  
    s->set_aec2(s, 1);           
    s->set_contrast(s, 0);       
    s->set_saturation(s, 0);     

    // ---------------------------------------------------
    // SERVER 1: API / CONTROL SERVER (PORT 80)
    // ---------------------------------------------------
    httpd_config_t config_api = HTTPD_DEFAULT_CONFIG();
    config_api.server_port = 80;
    config_api.ctrl_port = 32546; // Default UDP control port
    httpd_handle_t api_server = NULL;
    if (httpd_start(&api_server, &config_api) == ESP_OK) {
        httpd_uri_t cmd_uri = { .uri = "/control", .method = HTTP_GET, .handler = cmd_handler, .user_ctx = NULL };
        httpd_register_uri_handler(api_server, &cmd_uri);
        ESP_LOGI(TAG, "Control API started on Port 80");
    }

    // ---------------------------------------------------
    // SERVER 2: VIDEO STREAM SERVER (PORT 81)
    // ---------------------------------------------------
    httpd_config_t config_stream = HTTPD_DEFAULT_CONFIG();
    config_stream.server_port = 81;
    config_stream.ctrl_port = 32547; // MUST be different from Server 1
    httpd_handle_t stream_server = NULL;
    if (httpd_start(&stream_server, &config_stream) == ESP_OK) {
        httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
        httpd_register_uri_handler(stream_server, &stream_uri);
        ESP_LOGI(TAG, "Video Stream started on Port 81");
    }
}
