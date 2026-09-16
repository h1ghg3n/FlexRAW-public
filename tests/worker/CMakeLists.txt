add_executable(flexraw_worker_tests)

set_target_properties(flexraw_worker_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime"
)

target_sources(flexraw_worker_tests PRIVATE
    frame_codec_test.cpp
    frame_parser_test.cpp
    health_payload_codec_test.cpp
    job_status_contracts_test.cpp
    job_scheduler_test.cpp
    protocol_session_test.cpp
    remote_render_executor_test.cpp
    remote_render_orchestrator_test.cpp
    remote_render_request_mapper_test.cpp
    remote_render_integration_test.cpp
    shared_storage_locator_test.cpp
    resource_admission_test.cpp
    render_payload_codec_test.cpp
    worker_application_context_test.cpp
    worker_health_probe_test.cpp
    worker_test_main.cpp
    worker_path_resolver_test.cpp
)

target_link_libraries(flexraw_worker_tests PRIVATE
    flexraw_worker_app
    flexraw_worker_client
    flexraw_worker_network
    flexraw_worker_protocol
    flexraw_worker_runtime
    GTest::gtest
)

target_compile_definitions(flexraw_worker_tests PRIVATE
    FLEXRAW_WORKER_EXECUTABLE_PATH="$<TARGET_FILE:flexraw-worker>"
)

add_dependencies(flexraw_worker_tests flexraw-worker)

if(FLEXRAW_BUILD_DESKTOP)
    target_sources(flexraw_worker_tests PRIVATE
        remote_export_execution_adapter_test.cpp
        worker_health_probe_adapter_test.cpp
    )
    target_link_libraries(flexraw_worker_tests PRIVATE
        flexraw_worker_export_adapter
        flexraw_worker_health_adapter
    )
endif()

include(GoogleTest)

gtest_discover_tests(flexraw_worker_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        ENVIRONMENT "QT_PLUGIN_PATH=${FLEXRAW_QT_PLUGIN_PATH}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FLEXRAW_VCPKG_BIN_PATH}"
)

add_executable(flexraw_worker_network_tests)

set_target_properties(flexraw_worker_network_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/network"
)

target_sources(flexraw_worker_network_tests PRIVATE
    worker_server_test.cpp
    worker_test_main.cpp
)

target_link_libraries(flexraw_worker_network_tests PRIVATE
    flexraw_worker_network
    GTest::gtest
)

gtest_discover_tests(flexraw_worker_network_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        ENVIRONMENT "QT_PLUGIN_PATH=${FLEXRAW_QT_PLUGIN_PATH}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FLEXRAW_VCPKG_BIN_PATH}"
)
