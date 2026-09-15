add_executable(flexraw_core_tests)

set_target_properties(flexraw_core_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime"
)

target_sources(flexraw_core_tests PRIVATE
    catalog_database_test.cpp
    catalog_develop_repository_test.cpp
    catalog_folder_importer_test.cpp
    catalog_photo_repository_test.cpp
    catalog_project_repository_test.cpp
    catalog_preset_repository_test.cpp
    clipping_test.cpp
    color_test.cpp
    folder_scanner_test.cpp
    develop_test.cpp
    histogram_test.cpp
    measurement_test.cpp
    render_test.cpp
    develop_history_test.cpp
    export_test.cpp
    preview_cache_test.cpp
    raw_file_reader_test.cpp
    source_binding_test.cpp
    source_fingerprint_test.cpp
    source_path_test.cpp
    supported_extensions_test.cpp
    thumbnail_preview_test.cpp
)

target_link_libraries(flexraw_core_tests PRIVATE
    flexraw_core
    GTest::gtest_main
)

include(GoogleTest)

gtest_discover_tests(flexraw_core_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        ENVIRONMENT "QT_PLUGIN_PATH=${FLEXRAW_QT_PLUGIN_PATH}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FLEXRAW_VCPKG_BIN_PATH}"
)
