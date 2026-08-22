# facade - GUI-facing Catalog/Editor adapter boundary

add_library(flexraw_ui_facade STATIC)

target_sources(flexraw_ui_facade PRIVATE
    catalog_editor_facade.cpp
    catalog_editor_facade.h
)

target_include_directories(flexraw_ui_facade PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_ui_facade PUBLIC
    flexraw_core_orchestration
    Qt6::Core
    Qt6::Gui
)
