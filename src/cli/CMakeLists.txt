add_library(flexraw_cli_adapter STATIC)

set_target_properties(flexraw_cli_adapter PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
)

target_sources(flexraw_cli_adapter PRIVATE
    terminal_cli.cpp
    terminal_cli.h
)

target_include_directories(flexraw_cli_adapter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_cli_adapter PUBLIC
    flexraw_core_render_portable
)

add_executable(flexraw-cli main.cpp)

set_target_properties(flexraw-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/cli"
)

target_compile_definitions(flexraw-cli PRIVATE
    FLEXRAW_VERSION_STRING="${FLEXRAW_VERSION_STRING}"
)

target_link_libraries(flexraw-cli PRIVATE
    flexraw_cli_adapter
)

if(MSVC)
    target_compile_options(flexraw_cli_adapter PRIVATE /utf-8)
    target_compile_options(flexraw-cli PRIVATE /utf-8)
endif()
