idf_component_register(
    SRCS "main.c" "hid_host_task.c" "c6_uart.c" "kb_layouts.c" "led.c"
    INCLUDE_DIRS "."
    REQUIRES usb driver sdmmc esp_driver_sdmmc esp_driver_rmt efuse esp_hw_support
)
