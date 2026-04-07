idf_component_register(
    SRCS "at_cmd.c" "modem.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/main"
    REQUIRES driver freertos
)
