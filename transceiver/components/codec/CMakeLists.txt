idf_component_register(
    SRCS "opus_codec.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/main"
    REQUIRES esp_common freertos state esp-libopus esp_timer
)
