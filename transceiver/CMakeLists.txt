# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# esp_lcd は本プロジェクトで未使用。GCC 15.2.0 でのコンパイラICEを回避するため除外
set(EXCLUDE_COMPONENTS esp_lcd)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(transceiver)

# GCC 15.2.0 で esp-libopus 内部コードが -Wstringop-overflow を誤検知するため抑制
# また extensions.c がライブラリの CMakeLists.txt に含まれていないため追加
idf_component_get_property(libopus_lib esp-libopus COMPONENT_LIB)
target_compile_options(${libopus_lib} PRIVATE -Wno-stringop-overflow)
target_sources(${libopus_lib} PRIVATE
    "${CMAKE_SOURCE_DIR}/managed_components/esp-libopus/opus/src/extensions.c"
)
