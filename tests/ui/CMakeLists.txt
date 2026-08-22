add_executable(flexraw_ui_tests)

set_target_properties(flexraw_ui_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime"
)

target_sources(flexraw_ui_tests PRIVATE
    catalog_editor_facade_test.cpp
    catalog_command_service_test.cpp
    catalog_list_widget_test.cpp
    console_command_controller_test.cpp
    develop_panel_test.cpp
    develop_preview_controller_test.cpp
    export_command_service_test.cpp
    export_dialog_test.cpp
    export_settings_test.cpp
    folder_scan_controller_test.cpp
    mainwindow_catalog_pagination_test.cpp
    preview_widget_test.cpp
    raw_diagnostic_service_test.cpp
    source_resolution_widget_test.cpp
    ui_test_main.cpp
)

target_link_libraries(flexraw_ui_tests PRIVATE
    flexraw_ui_cli
    flexraw_ui_export
    flexraw_ui_mainwindow
    flexraw_worker_export_adapter
    GTest::gtest
    Qt6::Widgets
)

include(GoogleTest)

gtest_discover_tests(flexraw_ui_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        ENVIRONMENT "QT_PLUGIN_PATH=${FLEXRAW_QT_PLUGIN_PATH}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FLEXRAW_VCPKG_BIN_PATH}"
)
