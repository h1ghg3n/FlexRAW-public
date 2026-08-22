# color — 색 공간 변환 / ICC 프로파일 / lcms2 통합

add_library(flexraw_core_color STATIC)

target_sources(flexraw_core_color PRIVATE
    color.cpp
    color.h
)

target_include_directories(flexraw_core_color PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_color PUBLIC
    flexraw_core_types
    flexraw_core_util
    Qt6::Core
    Qt6::Gui
    lcms2::lcms2
)
