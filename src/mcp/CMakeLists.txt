add_library(flexraw_mcp_adapter STATIC)

target_sources(flexraw_mcp_adapter PRIVATE
    mcp_catalog_tools.cpp
    mcp_catalog_tools.h
    mcp_client_error_projection.cpp
    mcp_client_error_projection.h
    mcp_editor_tools.cpp
    mcp_editor_tools.h
    mcp_json_projection.cpp
    mcp_json_projection.h
    mcp_project_tools.cpp
    mcp_project_tools.h
    mcp_server.cpp
    mcp_server.h
    mcp_source_resolution_tools.cpp
    mcp_source_resolution_tools.h
    mcp_tool_result.h
)

target_include_directories(flexraw_mcp_adapter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_mcp_adapter
    PUBLIC
        flexraw_core_client
        Qt6::Core
)

add_executable(flexraw-mcp main.cpp)

set_target_properties(flexraw-mcp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mcp"
)

target_compile_definitions(flexraw-mcp PRIVATE
    FLEXRAW_VERSION_STRING="${FLEXRAW_VERSION_STRING}"
)

target_link_libraries(flexraw-mcp PRIVATE
    flexraw_app_mcp
    flexraw_mcp_adapter
    Qt6::Core
)

if(WIN32)
    qt_generate_deploy_app_script(
        TARGET flexraw-mcp
        OUTPUT_SCRIPT flexraw_mcp_deploy_script
        INCLUDE_PLUGIN_TYPES sqldrivers
        NO_TRANSLATIONS
    )
    add_custom_command(TARGET flexraw-mcp POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DQT_DEPLOY_PREFIX=$<TARGET_FILE_DIR:flexraw-mcp>
            -DQT_DEPLOY_BIN_DIR=.
            -DQT_DEPLOY_LIB_DIR=.
            -DQT_DEPLOY_PLUGINS_DIR=plugins
            -P ${flexraw_mcp_deploy_script}
        WORKING_DIRECTORY $<TARGET_FILE_DIR:flexraw-mcp>
        COMMENT "Deploying MCP runtime dependencies"
    )
endif()
