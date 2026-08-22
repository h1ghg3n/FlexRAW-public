# export — JPEG/TIFF/PNG/DNG 파이프라인

add_library(flexraw_core_export STATIC)

target_sources(flexraw_core_export PRIVATE
    export.cpp
    export.h
    export_options.h
)

target_include_directories(flexraw_core_export PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_export PUBLIC
    flexraw_core_color
    flexraw_core_develop
    flexraw_core_raw
    flexraw_core_types
    flexraw_core_util
    Qt6::Core
    Qt6::Gui
    Exiv2::exiv2lib
)
