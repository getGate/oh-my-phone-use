# Requirements must not depend on Kconfig (ESP-IDF resolves them before sdkconfig is loaded);
# only the source list is conditional.
set(srcs "")
if(CONFIG_PHONEUSE_HFP_ENABLE)
    list(APPEND srcs "hfp_client.c" "call_audio.c" "audio_codec.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES bt bt_stack esp_timer)
