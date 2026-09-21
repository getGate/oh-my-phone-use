# Requirements must not depend on Kconfig (ESP-IDF resolves them before sdkconfig is loaded);
# only the source list is conditional.
set(srcs "hid_device.c" "hid_keymap.c" "hid_motion.c")
if(CONFIG_PHONEUSE_HID_ENABLE)
    list(APPEND srcs "hid_ble.c")
else()
    list(APPEND srcs "hid_null.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private"
                       PRIV_REQUIRES esp_hid bt bt_stack)
