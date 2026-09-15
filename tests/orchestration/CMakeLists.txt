add_executable(flexraw_orchestration_tests)

set_target_properties(flexraw_orchestration_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime"
)

target_sources(flexraw_orchestration_tests PRIVATE
    catalog_orchestrator_test.cpp
    catalog_session_orchestrator_test.cpp
    catalog_thumbnail_orchestrator_test.cpp
    client_error_projection_test.cpp
    editor_client_projection_test.cpp
    editor_orchestrator_test.cpp
    export_orchestrator_test.cpp
    export_placement_test.cpp
    preview_orchestrator_test.cpp
    qt_source_resolution_event_source_test.cpp
    test_application.h
    worker_health_orchestrator_test.cpp
)

target_link_libraries(flexraw_orchestration_tests PRIVATE
    flexraw_core_orchestration
    flexraw_platform_api
    flexraw_test_support
    GTest::gtest_main
)

include(GoogleTest)

gtest_discover_tests(flexraw_orchestration_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FLEXRAW_VCPKG_BIN_PATH}"
)
