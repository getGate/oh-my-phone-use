# Requirements must not depend on Kconfig (ESP-IDF resolves them before sdkconfig is loaded);
# only the source list is conditional.
set(srcs "input_records.c")
if(CONFIG_PHONEUSE_INPUT_TOOL_ENABLE)
    list(APPEND srcs "input_player.c" "input_secret.c" "input_http.c" "boot_button.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES hid
                       PRIV_REQUIRES net esp_http_server nvs_flash esp_timer esp_driver_gpio)
