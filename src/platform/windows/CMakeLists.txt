# platform_windows - Windows OS capability adapters

add_library(flexraw_platform_windows STATIC)

target_sources(flexraw_platform_windows PRIVATE
    windows_process_memory_probe.cpp
    windows_process_memory_probe.h
    windows_system_memory_probe.cpp
    windows_system_memory_probe.h
)

target_include_directories(flexraw_platform_windows PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_platform_windows PUBLIC
    flexraw_platform_api
)

target_link_libraries(flexraw_platform_windows PRIVATE
    Psapi
)

set_target_properties(flexraw_platform_windows PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
