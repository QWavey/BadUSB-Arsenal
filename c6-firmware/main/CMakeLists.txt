idf_component_register(
    SRCS "main.c" "web_ui.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server driver
)
