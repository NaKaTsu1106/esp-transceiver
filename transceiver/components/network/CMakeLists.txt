idf_component_register(
    SRCS "tcp_client.c" "udp_client.c"
    INCLUDE_DIRS "."
    REQUIRES modem protocol freertos
)
