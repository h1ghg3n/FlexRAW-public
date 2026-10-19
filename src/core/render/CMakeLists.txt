# render - resolved single-RAW processing contract and local execution seam

if(FLEXRAW_BUILD_CLI OR (FLEXRAW_BUILD_TESTS AND FLEXRAW_BUILD_PORTABLE_TESTS))

add_library(flexraw_core_render_portable STATIC)

set_target_properties(flexraw_core_render_portable PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
)

target_sources(flexraw_core_render_portable PRIVATE
    portable_png_render_pipeline.cpp
    portable_png_render_pipeline.h
)

target_include_directories(flexraw_core_render_portable PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_render_portable
    PUBLIC
        flexraw_core_color_processing
        flexraw_core_develop_processing
        flexraw_core_export_file
        flexraw_core_export_png
        flexraw_core_raw_processing
        flexraw_core_types_neutral
)

if(MSVC)
    target_compile_options(flexraw_core_render_portable PRIVATE /utf-8)
endif()

endif()

if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER OR FLEXRAW_BUILD_MCP)

add_library(flexraw_core_render STATIC)

target_sources(flexraw_core_render PRIVATE
    local_render_executor.cpp
    local_render_executor.h
    render_contracts.h
    resolved_render_pipeline.cpp
    resolved_render_pipeline.h
)

target_include_directories(flexraw_core_render PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_render
    PUBLIC
        flexraw_core_export
        flexraw_core_measurement
        flexraw_core_types
        Qt6::Core
    PRIVATE
        flexraw_core_develop
        flexraw_core_export_qt_adapter
        flexraw_core_raw
        Qt6::Gui
)

endif()
