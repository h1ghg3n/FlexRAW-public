# worker_app - Worker process composition root

add_library(flexraw_worker_app STATIC)

target_sources(flexraw_worker_app PRIVATE
    worker_application_context.cpp
    worker_application_context.h
)

target_include_directories(flexraw_worker_app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_worker_app PUBLIC
    flexraw_core_render
    flexraw_platform_current
    flexraw_worker_admission
    flexraw_worker_network
    flexraw_worker_runtime
    Qt6::Core
    Qt6::Network
)
