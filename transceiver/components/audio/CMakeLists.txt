idf_component_register(
    SRCS "i2s_capture.c" "i2s_playback.c" "jitter_buf.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/main"
    REQUIRES esp_driver_i2s esp_driver_gpio freertos state
)
