# develop — neutral processing과 현재 Qt compatibility/analysis 경계

find_package(Threads REQUIRED)

add_library(flexraw_core_develop_processing STATIC)

set_target_properties(flexraw_core_develop_processing PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
)

target_sources(flexraw_core_develop_processing PRIVATE
    bounded_float32_neighborhood.cpp
    bounded_float32_neighborhood.h
    bounded_float32_neighborhood_detail.h
    bounded_float32_noise_reduction.cpp
    develop_params_validation.cpp
    develop_params_validation.h
    develop_processing.cpp
    develop_processing.h
    develop_processing_detail.h
    hsl_processing.cpp
    hsl_processing.h
)

target_include_directories(flexraw_core_develop_processing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_develop_processing PUBLIC
    flexraw_core_image
    flexraw_core_types_neutral
)

target_link_libraries(flexraw_core_develop_processing PRIVATE
    Threads::Threads
)

if(MSVC)
    target_compile_options(flexraw_core_develop_processing PRIVATE /utf-8)
endif()

if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER OR FLEXRAW_BUILD_MCP)

add_library(flexraw_core_develop STATIC)

target_sources(flexraw_core_develop PRIVATE
    clipping.cpp
    clipping.h
    develop.cpp
    develop.h
    develop_qt_adapter.cpp
    develop_qt_adapter.h
    develop_validation_adapter.cpp
    develop_validation_adapter.h
    histogram.cpp
    histogram.h
)

target_include_directories(flexraw_core_develop PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_develop PUBLIC
    flexraw_core_develop_processing
    flexraw_core_types
    flexraw_core_util
    Qt6::Core
    Qt6::Gui
)
endif()
