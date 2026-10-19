add_executable(flexraw_mcp_tests
    mcp_test_main.cpp
    mcp_application_context_test.cpp
    mcp_export_tools_test.cpp
    mcp_server_test.cpp
    mcp_source_resolution_tools_test.cpp
)

set_target_properties(flexraw_mcp_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime"
)

target_link_libraries(flexraw_mcp_tests PRIVATE
    flexraw_mcp_adapter
    flexraw_app_mcp
    flexraw_core_catalog
    GTest::gtest
    Qt6::Core
)

target_compile_definitions(flexraw_mcp_tests PRIVATE
    FLEXRAW_MCP_EXECUTABLE_PATH="$<TARGET_FILE:flexraw-mcp>"
)

add_dependencies(flexraw_mcp_tests flexraw-mcp)

include(GoogleTest)
gtest_discover_tests(flexraw_mcp_tests
    PROPERTIES
        ENVIRONMENT "QT_PLUGIN_PATH=${FLEXRAW_QT_PLUGIN_PATH}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FLEXRAW_VCPKG_BIN_PATH}"
)
