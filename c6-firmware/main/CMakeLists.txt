idf_component_register(
    SRCS "main.c" "web_ui.c" "badusb_ui.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server driver spiffs esp_timer spi_flash
)

# Build a SPIFFS image from ../webui-fs/ and flash it into the "storage"
# partition on every `idf.py flash`. Files there are served at "/" by
# badusb_ui.c via a directory-mapped handler — same layout as the SD
# root the reference firmware uses ("/index.html", "/style.css",
# "/script.js", "/languages/*.json"), so drop-in replacements are just
# an idf.py flash away.
spiffs_create_partition_image(
    storage
    "${CMAKE_CURRENT_LIST_DIR}/../webui-fs"
    FLASH_IN_PROJECT
)
