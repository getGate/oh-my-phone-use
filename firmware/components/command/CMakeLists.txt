idf_component_register(SRCS "command_parser.c" "serial_console.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES hid esp_driver_uart)
