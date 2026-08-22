add_executable(flexraw_worker_render_benchmark)
add_executable(flexraw_worker_batch_benchmark)

if(FLEXRAW_BUILD_DESKTOP)
    add_executable(flexraw_export_placement_benchmark)
endif()

target_sources(flexraw_worker_render_benchmark PRIVATE
    render_comparison.cpp
)

target_sources(flexraw_worker_batch_benchmark PRIVATE
    batch_render_comparison.cpp
)

if(TARGET flexraw_export_placement_benchmark)
    target_sources(flexraw_export_placement_benchmark PRIVATE
        export_placement_comparison.cpp
    )
endif()

target_link_libraries(flexraw_worker_render_benchmark PRIVATE
    flexraw_core_measurement
    flexraw_core_render
    flexraw_worker_app
    flexraw_worker_protocol
    flexraw_worker_runtime
    Qt6::Core
    Qt6::Gui
    Qt6::Network
)

target_link_libraries(flexraw_worker_batch_benchmark PRIVATE
    flexraw_core_measurement
    flexraw_core_render
    flexraw_core_util
    flexraw_platform_current
    flexraw_worker_runtime
    Qt6::Core
    Qt6::Gui
)

if(TARGET flexraw_export_placement_benchmark)
    target_link_libraries(flexraw_export_placement_benchmark PRIVATE
        flexraw_core_measurement
        flexraw_core_orchestration
        flexraw_core_util
        flexraw_platform_current
        flexraw_worker_client
        flexraw_worker_export_adapter
        Qt6::Core
        Qt6::Gui
    )
endif()
