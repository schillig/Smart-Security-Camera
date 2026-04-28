idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES app_update esp_wifi nvs_flash esp_http_server lwip esp32-camera led_strip esp_event)
