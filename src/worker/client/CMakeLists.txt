# worker_client - synchronous manual-endpoint render protocol client

add_library(flexraw_worker_client STATIC)

target_sources(flexraw_worker_client PRIVATE
    remote_render_contracts.h
    remote_render_executor.cpp
    remote_render_executor.h
    remote_render_orchestrator.cpp
    remote_render_orchestrator.h
    remote_render_preflight.cpp
    remote_render_preflight.h
    remote_render_request_mapper.cpp
    remote_render_request_mapper.h
    shared_storage_locator.cpp
    shared_storage_locator.h
    worker_health_probe.cpp
    worker_health_probe.h
)

target_include_directories(flexraw_worker_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_worker_client
    PUBLIC
        flexraw_core_render
        flexraw_core_types
        Qt6::Core
    PRIVATE
        flexraw_worker_protocol
        flexraw_worker_render_messages
        Qt6::Network
)

set_target_properties(flexraw_worker_client PROPERTIES
    AUTOMOC ON
    AUTOUIC OFF
    AUTORCC OFF
)

if(FLEXRAW_BUILD_DESKTOP)
    add_library(flexraw_worker_health_adapter STATIC)

    target_sources(flexraw_worker_health_adapter PRIVATE
        worker_health_probe_adapter.cpp
        worker_health_probe_adapter.h
    )

    target_include_directories(flexraw_worker_health_adapter PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(flexraw_worker_health_adapter
        PUBLIC
            flexraw_core_orchestration
        PRIVATE
            flexraw_worker_client
            Qt6::Core
    )

    add_library(flexraw_worker_export_adapter STATIC)

    target_sources(flexraw_worker_export_adapter PRIVATE
        remote_export_execution_adapter.cpp
        remote_export_execution_adapter.h
    )

    target_include_directories(flexraw_worker_export_adapter PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(flexraw_worker_export_adapter
        PUBLIC
            flexraw_core_orchestration
        PRIVATE
            flexraw_worker_client
            Qt6::Core
    )
endif()
