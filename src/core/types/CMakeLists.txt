# types — neutral value contract와 현재 Qt 기반 도메인 타입

add_library(flexraw_core_types_neutral INTERFACE)

target_sources(flexraw_core_types_neutral INTERFACE
    develop_params.h
    hsl_params.h
    result.h
)

target_include_directories(flexraw_core_types_neutral INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER OR FLEXRAW_BUILD_MCP)
add_library(flexraw_core_types STATIC)

target_sources(flexraw_core_types PRIVATE
    error.h
    file_types.h
    operation_types.h
    photo_identity.h
    types.cpp
)

target_include_directories(flexraw_core_types PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_types PUBLIC
    flexraw_core_types_neutral
    Qt6::Core
)
endif()
