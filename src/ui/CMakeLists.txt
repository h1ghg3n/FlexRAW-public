# ui — 모듈별 STATIC library 구성
# flexraw_ui 는 INTERFACE library 로, 모든 sub 모듈을 묶어 노출한다.

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_subdirectory(panels)
add_subdirectory(catalog)
add_subdirectory(library)
add_subdirectory(editor)
add_subdirectory(import)
add_subdirectory(settings)
add_subdirectory(cli)
add_subdirectory(facade)
add_subdirectory(export)
add_subdirectory(mainwindow)

add_library(flexraw_ui INTERFACE)
target_link_libraries(flexraw_ui INTERFACE
    flexraw_ui_panels
    flexraw_ui_mainwindow
    flexraw_ui_catalog
    flexraw_ui_library
    flexraw_ui_editor
    flexraw_ui_import
    flexraw_ui_settings
    flexraw_ui_cli
    flexraw_ui_facade
    flexraw_ui_export
    flexraw_core
)
