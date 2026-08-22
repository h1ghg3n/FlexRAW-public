# export — graphical raster export adapter

add_library(flexraw_ui_export STATIC)

target_sources(flexraw_ui_export PRIVATE
    export_dialog.cpp
    export_dialog.h
)

target_include_directories(flexraw_ui_export PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_ui_export PUBLIC
    flexraw_core_export
    flexraw_core_orchestration
    flexraw_ui_settings
    flexraw_worker_client
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
