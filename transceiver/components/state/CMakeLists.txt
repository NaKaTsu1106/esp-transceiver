idf_component_register(
    SRCS "state_machine.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/main"
    REQUIRES freertos protocol esp_adc pmic
)
