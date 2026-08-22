# catalog — 카탈로그 매니저 UI

add_library(flexraw_ui_catalog STATIC)

target_sources(flexraw_ui_catalog PRIVATE
    catalog.cpp
    catalog_list_widget.h
    source_resolution_widget.cpp
    source_resolution_widget.h
)

target_include_directories(flexraw_ui_catalog PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_ui_catalog PUBLIC
    flexraw_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
