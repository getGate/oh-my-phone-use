idf_component_register(SRCS "hid_device.c" "hid_ble.c" "hid_keymap.c" "hid_motion.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private"
                       PRIV_REQUIRES esp_hid bt bt_stack)
