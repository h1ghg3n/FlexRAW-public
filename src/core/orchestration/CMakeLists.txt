# orchestration — cross-module use case ordering and async lifecycle

add_library(flexraw_core_orchestration STATIC)

target_sources(flexraw_core_orchestration PRIVATE
    catalog_contracts.h
    catalog_orchestrator.cpp
    catalog_orchestrator.h
    catalog_thumbnail_contracts.h
    catalog_thumbnail_orchestrator.cpp
    catalog_thumbnail_orchestrator.h
    catalog_thumbnail_pipeline.cpp
    catalog_thumbnail_pipeline.h
    editor_contracts.h
    editor_client_projection.cpp
    editor_client_projection.h
    editor_orchestrator.cpp
    editor_orchestrator.h
    export_contracts.h
    export_execution_port.h
    export_orchestrator.cpp
    export_orchestrator.h
    export_pipeline.cpp
    export_pipeline.h
    preview_contracts.h
    preview_orchestrator.cpp
    preview_orchestrator.h
    preview_pipeline.cpp
    preview_pipeline.h
    preview_pipeline_worker.cpp
    preview_pipeline_worker.h
)

target_include_directories(flexraw_core_orchestration PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_orchestration
    PUBLIC
        flexraw_core_client
        flexraw_core_types
        flexraw_core_develop
        flexraw_core_export
        flexraw_core_history
        flexraw_core_catalog
        Qt6::Core
        Qt6::Gui
    PRIVATE
        flexraw_core_preview
        flexraw_core_util
        flexraw_platform_api
        Qt6::Concurrent
)
