idf_component_register(
    SRCS "main.c" "loopback_test.c" "lte_test.c"
    INCLUDE_DIRS "."
    REQUIRES pmic modem network protocol audio codec state freertos
)
