add_executable(flexraw_worker_tests)

set_target_properties(flexraw_worker_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime"
)

target_sources(flexraw_worker_tests PRIVATE
    frame_codec_test.cpp
    frame_parser_test.cpp
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
    worker_server_test.cpp
    worker_application_context_test.cpp
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

if(FLEXRAW_BUILD_DESKTOP)
    target_sources(flexraw_worker_tests PRIVATE
        remote_export_execution_adapter_test.cpp
    )
    target_link_libraries(flexraw_worker_tests PRIVATE
        flexraw_worker_export_adapter
    )
endif()

include(GoogleTest)

gtest_discover_tests(flexraw_worker_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        ENVIRONMENT "QT_PLUGIN_PATH=${FLEXRAW_QT_PLUGIN_PATH}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${FLEXRAW_VCPKG_BIN_PATH}"
)
