# worker_render_messages - shared render payload mapping for client and server

add_library(flexraw_worker_render_messages STATIC)

target_sources(flexraw_worker_render_messages PRIVATE
    render_message_contracts.h
    render_payload_codec.cpp
    render_payload_codec.h
)

target_include_directories(flexraw_worker_render_messages PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_worker_render_messages
    PUBLIC
        flexraw_core_export
        flexraw_core_measurement
        flexraw_core_types
        Qt6::Core
    PRIVATE
        flexraw_core_develop
        flexraw_worker_protocol
)

set_target_properties(flexraw_worker_render_messages PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

# worker_runtime_port - Server Adapter가 소비하는 specific Render Worker Runtime contract

add_library(flexraw_worker_runtime_port INTERFACE)

target_sources(flexraw_worker_runtime_port INTERFACE
    render_job_identity.h
    render_worker_runtime_port.h
    runtime_observation_contracts.h
)

target_include_directories(flexraw_worker_runtime_port INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_worker_runtime_port INTERFACE
    flexraw_core_measurement
    flexraw_core_render
    Qt6::Core
)

if(FLEXRAW_BUILD_WORKER)
    # worker_runtime - Worker-root path resolution, runner and bounded scheduler

    add_library(flexraw_worker_runtime STATIC)

    target_sources(flexraw_worker_runtime PRIVATE
        job_status_contracts.h
        job_scheduler.cpp
        job_scheduler.h
        render_job_identity.h
        render_job_runner.cpp
        render_job_runner.h
        render_worker_runtime_facade.cpp
        render_worker_runtime_facade.h
        runtime_observation_contracts.h
        worker_path_resolver.cpp
        worker_path_resolver.h
    )

    target_include_directories(flexraw_worker_runtime PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(flexraw_worker_runtime
        PUBLIC
            flexraw_core_render
            flexraw_worker_runtime_port
            Qt6::Core
    )

    set_target_properties(flexraw_worker_runtime PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
endif()
