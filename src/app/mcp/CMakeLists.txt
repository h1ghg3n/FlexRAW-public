add_library(flexraw_app_mcp STATIC)

target_sources(flexraw_app_mcp PRIVATE
    mcp_application_context.cpp
    mcp_application_context.h
)

target_include_directories(flexraw_app_mcp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_app_mcp
    PUBLIC
        flexraw_core_client
    PRIVATE
        flexraw_product_runtime
        flexraw_core_orchestration
        Qt6::Core
)
