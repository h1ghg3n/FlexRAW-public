# editor — RAW 보정 파라미터 패널 + 히스토그램

add_library(flexraw_ui_editor STATIC)

target_sources(flexraw_ui_editor PRIVATE
    develop_panel.cpp
    develop_panel.h
    develop_preview_controller.cpp
    develop_preview_controller.h
    editor.cpp
    histogram_widget.cpp
    histogram_widget.h
    preview_widget.h
)

target_include_directories(flexraw_ui_editor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_ui_editor PUBLIC
    flexraw_core
    Qt6::Core
    Qt6::Concurrent
    Qt6::Gui
    Qt6::Widgets
)
