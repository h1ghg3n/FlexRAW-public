# app — production Composition Root

add_library(flexraw_app STATIC)

target_sources(flexraw_app PRIVATE
    application_context.cpp
    application_context.h
    managed_catalog_session.cpp
    managed_catalog_session.h
)

target_include_directories(flexraw_app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_app
    PUBLIC
        flexraw_ui_mainwindow
    PRIVATE
        flexraw_core_orchestration
        flexraw_core_preview
        flexraw_platform_current
        flexraw_ui_facade
        flexraw_worker_export_adapter
        Qt6::Core
)
