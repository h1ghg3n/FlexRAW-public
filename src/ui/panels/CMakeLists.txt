# panels — 4방향 패널 시스템 (커스텀 구현, QDockWidget 미사용)

add_library(flexraw_ui_panels STATIC)

target_sources(flexraw_ui_panels PRIVATE
    panels.cpp
)

target_include_directories(flexraw_ui_panels PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_ui_panels PUBLIC
    flexraw_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
