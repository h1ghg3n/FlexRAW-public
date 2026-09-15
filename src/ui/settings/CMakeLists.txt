# settings — 설정 탭 UI

add_library(flexraw_ui_settings STATIC)

target_sources(flexraw_ui_settings PRIVATE
    editor_settings.cpp
    editor_settings.h
    export_defaults_page.cpp
    export_defaults_page.h
    general_settings_page.cpp
    general_settings_page.h
    qt_catalog_startup_settings_adapter.cpp
    qt_catalog_startup_settings_adapter.h
    qt_export_settings_adapter.cpp
    qt_export_settings_adapter.h
    settings_dialog.cpp
    settings_dialog.h
    qt_worker_profile_settings_adapter.cpp
    qt_worker_profile_settings_adapter.h
    qt_worker_health_event_adapter.cpp
    qt_worker_health_event_adapter.h
    worker_profiles_page.cpp
    worker_profiles_page.h
    worker_health_panel.cpp
    worker_health_panel.h
)

target_include_directories(flexraw_ui_settings PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_ui_settings PUBLIC
    flexraw_core_client
    flexraw_ui_editor
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

target_link_libraries(flexraw_ui_settings PRIVATE
    flexraw_core_orchestration
)
