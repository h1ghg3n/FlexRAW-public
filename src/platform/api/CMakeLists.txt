# platform_api - Qt-free OS capability contracts

add_library(flexraw_platform_api INTERFACE)

target_include_directories(flexraw_platform_api INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(flexraw_platform_api INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(flexraw_platform_api INTERFACE /utf-8)
endif()
