# render - resolved single-RAW processing contract and local execution seam

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
        flexraw_core_raw
        Qt6::Gui
)
