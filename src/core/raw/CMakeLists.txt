# raw — neutral LibRaw decode와 Qt metadata/thumbnail compatibility 경계

add_library(flexraw_core_raw_processing STATIC)

set_target_properties(flexraw_core_raw_processing PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
)

target_sources(flexraw_core_raw_processing PRIVATE
    raw_rgba8_bridge.cpp
    raw_rgba8_bridge.h
    raw_processing.cpp
    raw_processing.h
)

target_include_directories(flexraw_core_raw_processing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_raw_processing PUBLIC
    flexraw_core_image
    flexraw_core_types_neutral
)

target_link_libraries(flexraw_core_raw_processing PRIVATE
    libraw::raw_r
)

if(MSVC)
    target_compile_options(flexraw_core_raw_processing PRIVATE /utf-8)
endif()

if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER OR FLEXRAW_BUILD_MCP)

add_library(flexraw_core_raw STATIC)

target_sources(flexraw_core_raw PRIVATE
    raw_file_reader.cpp
    raw_file_reader.h
)

target_include_directories(flexraw_core_raw PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_raw PUBLIC
    flexraw_core_raw_processing
    flexraw_core_types
    flexraw_core_util
    Qt6::Core
    Qt6::Gui
)

target_link_libraries(flexraw_core_raw PRIVATE
    libraw::raw_r
)

endif()
