idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES pmic modem network protocol audio codec state freertos
)
