# PhoneUse HID firmware: ESP32-PICO-KIT-1 as a Bluetooth HID mouse/keyboard for iPhone AssistiveTouch.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the components main depends on (keeps the build small and fast).
idf_build_set_property(MINIMAL_BUILD ON)
project(phoneuse_hid)
