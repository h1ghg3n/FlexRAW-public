# export — neutral recipe와 Qt processing/publication compatibility 경계

add_library(flexraw_core_export_processing STATIC)

set_target_properties(flexraw_core_export_processing PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
)

target_sources(flexraw_core_export_processing PRIVATE
    export_options.h
    export_processing.cpp
    export_processing.h
)

target_include_directories(flexraw_core_export_processing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(MSVC)
    target_compile_options(flexraw_core_export_processing PRIVATE /utf-8)
endif()

if(FLEXRAW_BUILD_CLI OR (FLEXRAW_BUILD_TESTS AND FLEXRAW_BUILD_PORTABLE_TESTS))

add_library(flexraw_core_export_png STATIC)

set_target_properties(flexraw_core_export_png PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
)

target_sources(flexraw_core_export_png PRIVATE
    png_encoder.cpp
    png_encoder.h
)

target_include_directories(flexraw_core_export_png PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_export_png
    PUBLIC
        flexraw_core_image
        flexraw_core_types_neutral
    PRIVATE
        PNG::PNG
)

add_library(flexraw_core_export_file STATIC)

set_target_properties(flexraw_core_export_file PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
)

target_sources(flexraw_core_export_file PRIVATE
    png_artifact_publisher.cpp
    png_artifact_publisher.h
)

target_include_directories(flexraw_core_export_file PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_export_file PUBLIC
    flexraw_core_export_png
    flexraw_core_types_neutral
)

if(MSVC)
    target_compile_options(flexraw_core_export_png PRIVATE /utf-8)
    target_compile_options(flexraw_core_export_file PRIVATE /utf-8)
endif()

endif()

if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER OR FLEXRAW_BUILD_MCP)

add_library(flexraw_core_export_qt_adapter STATIC)

target_sources(flexraw_core_export_qt_adapter PRIVATE
    export_qt_adapter.cpp
    export_qt_adapter.h
)

target_include_directories(flexraw_core_export_qt_adapter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_export_qt_adapter
    PUBLIC
        flexraw_core_export_processing
        flexraw_core_types
        Qt6::Core
        Qt6::Gui
    PRIVATE
        flexraw_core_color
        flexraw_core_raw
)

add_library(flexraw_core_export STATIC)

target_sources(flexraw_core_export PRIVATE
    export.cpp
    export.h
)

target_include_directories(flexraw_core_export PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_export
    PUBLIC
        flexraw_core_export_processing
        flexraw_core_types
        Qt6::Core
        Qt6::Gui
    PRIVATE
        flexraw_core_develop
        flexraw_core_export_qt_adapter
        flexraw_core_raw
        flexraw_core_util
        Exiv2::exiv2lib
)

endif()
