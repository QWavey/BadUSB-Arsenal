idf_component_register(
    SRCS "main.c" "hid_host_task.c"
    INCLUDE_DIRS "."
    REQUIRES usb
)
