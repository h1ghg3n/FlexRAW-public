# image — concrete neutral processing image views

add_library(flexraw_core_image STATIC)

set_target_properties(flexraw_core_image PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
)

target_sources(flexraw_core_image PRIVATE
    owned_rgba8_image.cpp
    owned_rgba8_image.h
    rgba8_image_view.cpp
    rgba8_image_view.h
)

target_include_directories(flexraw_core_image PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(MSVC)
    target_compile_options(flexraw_core_image PRIVATE /utf-8)
endif()
