add_executable(flexraw_app_tests)

set_target_properties(flexraw_app_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime"
)

target_sources(flexraw_app_tests PRIVATE
    managed_catalog_session_test.cpp
)

target_link_libraries(flexraw_app_tests PRIVATE
    flexraw_app
    flexraw_core_orchestration
    GTest::gtest_main
)

include(GoogleTest)

gtest_discover_tests(flexraw_app_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        ENVIRONMENT "QT_PLUGIN_PATH=${FLEXRAW_QT_PLUGIN_PATH}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FLEXRAW_VCPKG_BIN_PATH}"
)
