# lens — Lensfun 렌즈 보정

add_library(flexraw_core_lens STATIC)

target_sources(flexraw_core_lens PRIVATE
    lens.cpp
)

target_include_directories(flexraw_core_lens PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_lens PUBLIC
    flexraw_core_types
    flexraw_core_util
    Qt6::Core
    ${OpenCV_LIBS}
)
