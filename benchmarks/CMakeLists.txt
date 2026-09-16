add_subdirectory(render)
add_subdirectory(catalog)

set(FLEXRAW_BENCHMARK_VALIDATION_TARGETS
    flexraw_catalog_pagination_benchmark
    flexraw_develop_baseline
    flexraw_resolved_render_smoke
)

if(FLEXRAW_BUILD_WORKER)
    add_subdirectory(worker)
    list(APPEND FLEXRAW_BENCHMARK_VALIDATION_TARGETS
        flexraw_worker_render_benchmark
        flexraw_worker_batch_benchmark
    )
    if(TARGET flexraw_export_placement_benchmark)
        list(APPEND FLEXRAW_BENCHMARK_VALIDATION_TARGETS flexraw_export_placement_benchmark)
    endif()
endif()

foreach(FLEXRAW_BENCHMARK_TEST_TARGET
        flexraw_core_tests
        flexraw_orchestration_tests
        flexraw_ui_tests
        flexraw_app_tests
        flexraw_worker_tests)
    if(TARGET ${FLEXRAW_BENCHMARK_TEST_TARGET})
        list(APPEND FLEXRAW_BENCHMARK_VALIDATION_TARGETS ${FLEXRAW_BENCHMARK_TEST_TARGET})
    endif()
endforeach()

add_custom_target(flexraw_benchmark_validation DEPENDS ${FLEXRAW_BENCHMARK_VALIDATION_TARGETS})
