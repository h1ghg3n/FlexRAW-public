# platform_linux - Linux OS capability adapters

add_library(flexraw_platform_linux STATIC)

target_sources(flexraw_platform_linux PRIVATE
    linux_path_identity_service.cpp
    linux_path_identity_service.h
    linux_process_memory_probe.cpp
    linux_process_memory_probe.h
    linux_system_memory_probe.cpp
    linux_system_memory_probe.h
)

target_include_directories(flexraw_platform_linux PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_platform_linux PUBLIC
    flexraw_platform_api
)

set_target_properties(flexraw_platform_linux PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
