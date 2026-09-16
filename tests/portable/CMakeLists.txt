add_executable(flexraw_portable_contract_tests)

target_sources(flexraw_portable_contract_tests PRIVATE
    client_surface_contract_test.cpp
    display_frame_test.cpp
)

target_link_libraries(flexraw_portable_contract_tests PRIVATE
    flexraw_core_client
    GTest::gtest_main
)

if(MSVC)
    target_compile_options(flexraw_portable_contract_tests PRIVATE /utf-8)
endif()

include(GoogleTest)

gtest_discover_tests(flexraw_portable_contract_tests
    DISCOVERY_MODE PRE_TEST
)
