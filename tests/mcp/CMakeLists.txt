add_executable(flexraw_mcp_tests
    mcp_application_context_test.cpp
    mcp_server_test.cpp
    mcp_source_resolution_tools_test.cpp
)

target_link_libraries(flexraw_mcp_tests PRIVATE
    flexraw_mcp_adapter
    flexraw_app_mcp
    flexraw_core_catalog
    GTest::gtest_main
    Qt6::Core
)

target_compile_definitions(flexraw_mcp_tests PRIVATE
    FLEXRAW_MCP_EXECUTABLE_PATH="$<TARGET_FILE:flexraw-mcp>"
)

include(GoogleTest)
gtest_discover_tests(flexraw_mcp_tests)
