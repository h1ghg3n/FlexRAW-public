add_executable(flexraw_platform_tests)

set_target_properties(flexraw_platform_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime"
)

target_sources(flexraw_platform_tests PRIVATE
    path_identity_service_test.cpp
)

target_link_libraries(flexraw_platform_tests PRIVATE
    flexraw_platform_current
    GTest::gtest_main
)

include(GoogleTest)

gtest_discover_tests(flexraw_platform_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FLEXRAW_VCPKG_BIN_PATH}"
)
