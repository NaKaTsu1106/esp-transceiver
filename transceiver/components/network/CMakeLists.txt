idf_component_register(
    SRCS "tcp_client.c" "udp_client.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/main"
    REQUIRES modem protocol state freertos esp_hw_support
)
