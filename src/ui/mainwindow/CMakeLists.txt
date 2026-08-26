# mainwindow — 메인 윈도우, ui 모듈 조립

add_library(flexraw_ui_mainwindow STATIC)

target_sources(flexraw_ui_mainwindow PRIVATE
    folder_scan_controller.cpp
    folder_scan_controller.h
    mainwindow.cpp
    mainwindow.h
    qt_activity_adapter.cpp
    qt_activity_adapter.h
)

target_include_directories(flexraw_ui_mainwindow PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_ui_mainwindow PUBLIC
    flexraw_core
    flexraw_core_orchestration
    flexraw_ui_catalog
    flexraw_ui_cli
    flexraw_ui_editor
    flexraw_ui_export
    flexraw_ui_facade
    flexraw_ui_settings
    flexraw_worker_client
    Qt6::Core
    Qt6::Concurrent
    Qt6::Gui
    Qt6::Widgets
)
