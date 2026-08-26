# settings — 설정 탭 UI

add_library(flexraw_ui_settings STATIC)

target_sources(flexraw_ui_settings PRIVATE
    editor_settings.cpp
    editor_settings.h
    export_settings.cpp
    export_settings.h
    settings_dialog.cpp
    settings_dialog.h
)

target_include_directories(flexraw_ui_settings PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_ui_settings PUBLIC
    flexraw_core_export
    flexraw_ui_editor
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
