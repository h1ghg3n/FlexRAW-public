# worker_admission - local or Resource Router render admission boundary

add_library(flexraw_worker_admission STATIC)

target_sources(flexraw_worker_admission PRIVATE
    admission_render_job_runner.cpp
    admission_render_job_runner.h
    local_memory_admission.cpp
    local_memory_admission.h
    render_resource_admission.h
    resource_router_admission.cpp
    resource_router_admission.h
)

target_include_directories(flexraw_worker_admission PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_worker_admission PUBLIC
    flexraw_platform_api
    flexraw_worker_runtime
    Qt6::Core
    Qt6::Network
    PRIVATE
        flexraw_core_util
)

set_target_properties(flexraw_worker_admission PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

if(MSVC)
    target_compile_options(flexraw_worker_admission PRIVATE /utf-8)
endif()
