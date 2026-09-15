# facade - GUI-facing Catalog/Editor adapter boundary

add_library(flexraw_ui_facade STATIC)

target_sources(flexraw_ui_facade PRIVATE
    catalog_editor_facade.cpp
    catalog_editor_facade.h
    catalog_photo_page_qt_adapter.cpp
    catalog_photo_page_qt_adapter.h
    display_frame_qt_adapter.cpp
    display_frame_qt_adapter.h
    qt_catalog_thumbnail_event_adapter.cpp
    qt_catalog_thumbnail_event_adapter.h
    qt_editor_event_adapter.cpp
    qt_editor_event_adapter.h
    qt_folder_import_event_adapter.cpp
    qt_folder_import_event_adapter.h
    qt_preview_presentation_event_adapter.cpp
    qt_preview_presentation_event_adapter.h
)

target_include_directories(flexraw_ui_facade PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_ui_facade PUBLIC
    flexraw_core_client
    flexraw_core_orchestration
    Qt6::Core
    Qt6::Gui
)
