# cli — CLI 패널 + 콘솔 모드 전환

add_library(flexraw_ui_cli STATIC)

target_sources(flexraw_ui_cli PRIVATE
    catalog_command_service.cpp
    catalog_command_service.h
    cli.cpp
    console_command_controller.cpp
    console_command_controller.h
    console_mode_widget.h
    export_command_service.cpp
    export_command_service.h
    raw_diagnostic_service.cpp
    raw_diagnostic_service.h
)

target_include_directories(flexraw_ui_cli PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_ui_cli PUBLIC
    flexraw_core_catalog
    flexraw_core_client
    flexraw_core_export
    flexraw_core_orchestration
    flexraw_core_raw
    Qt6::Core
    Qt6::Concurrent
    Qt6::Gui
    Qt6::Widgets
)
