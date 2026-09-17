idf_component_register(
    SRCS "main.c" "hid_host_task.c" "c6_uart.c"
    INCLUDE_DIRS "."
    REQUIRES usb driver sdmmc esp_driver_sdmmc
)
