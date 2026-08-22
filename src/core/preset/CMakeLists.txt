# preset — 프리셋 저장/불러오기, Lightroom .xmp 호환

add_library(flexraw_core_preset STATIC)

target_sources(flexraw_core_preset PRIVATE
    preset.cpp
    preset.h
)

target_include_directories(flexraw_core_preset PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_preset PUBLIC
    flexraw_core_types
    flexraw_core_util
    Qt6::Core
)
