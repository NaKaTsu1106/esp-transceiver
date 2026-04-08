idf_component_register(
    SRCS "axp2101.c" "led.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/main"
    REQUIRES esp_driver_i2c freertos
)
