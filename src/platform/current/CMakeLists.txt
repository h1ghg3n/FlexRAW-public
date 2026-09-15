# platform_current - configured host OS adapter factory

if(WIN32)
    add_subdirectory(../windows windows)
    set(FLEXRAW_PLATFORM_CURRENT_ADAPTER flexraw_platform_windows)
    set(FLEXRAW_PLATFORM_CURRENT_DEFINE FLEXRAW_PLATFORM_CURRENT_WINDOWS)
elseif(UNIX AND NOT APPLE)
    add_subdirectory(../linux linux)
    set(FLEXRAW_PLATFORM_CURRENT_ADAPTER flexraw_platform_linux)
    set(FLEXRAW_PLATFORM_CURRENT_DEFINE FLEXRAW_PLATFORM_CURRENT_LINUX)
else()
    message(FATAL_ERROR "Flexraw has no current Platform adapter for this target OS.")
endif()

add_library(flexraw_platform_current STATIC)

target_sources(flexraw_platform_current PRIVATE
    current_path_identity_service.cpp
    current_path_identity_service.h
    current_process_memory_probe.cpp
    current_process_memory_probe.h
    current_system_memory_probe.cpp
    current_system_memory_probe.h
)

target_include_directories(flexraw_platform_current PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(flexraw_platform_current PRIVATE
    ${FLEXRAW_PLATFORM_CURRENT_DEFINE}
)

target_link_libraries(flexraw_platform_current PUBLIC
    flexraw_platform_api
    ${FLEXRAW_PLATFORM_CURRENT_ADAPTER}
)

set_target_properties(flexraw_platform_current PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
