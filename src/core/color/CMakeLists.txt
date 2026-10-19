# color — neutral ICC processing과 Qt compatibility adapter

add_library(flexraw_core_color_processing STATIC)

set_target_properties(flexraw_core_color_processing PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
)

target_sources(flexraw_core_color_processing PRIVATE
    color_processing.cpp
    color_processing.h
)

target_include_directories(flexraw_core_color_processing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_color_processing PUBLIC
    flexraw_core_image
    flexraw_core_types_neutral
)

target_link_libraries(flexraw_core_color_processing PRIVATE
    lcms2::lcms2
)

if(MSVC)
    target_compile_options(flexraw_core_color_processing PRIVATE /utf-8)
endif()

if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER OR FLEXRAW_BUILD_MCP)

add_library(flexraw_core_color STATIC)

target_sources(flexraw_core_color PRIVATE
    color_qt_adapter.cpp
    color_qt_adapter.h
)

target_include_directories(flexraw_core_color PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_color PUBLIC
    flexraw_core_color_processing
    flexraw_core_types
    flexraw_core_util
    Qt6::Core
    Qt6::Gui
)
endif()
