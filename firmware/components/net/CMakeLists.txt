# Requirements must not depend on Kconfig (ESP-IDF resolves them before sdkconfig is loaded);
# only the source list is conditional. The espressif/mdns dependency comes from idf_component.yml.
set(srcs "")
if(CONFIG_PHONEUSE_INPUT_TOOL_ENABLE)
    list(APPEND srcs "wifi_link.c" "mdns_advert.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash)
