add_executable(flexraw_cli_tests)

target_sources(flexraw_cli_tests PRIVATE
    terminal_cli_test.cpp
)

target_link_libraries(flexraw_cli_tests PRIVATE
    flexraw_cli_adapter
    GTest::gtest_main
)

if(MSVC)
    target_compile_options(flexraw_cli_tests PRIVATE /utf-8)
endif()

include(GoogleTest)

gtest_discover_tests(flexraw_cli_tests
    DISCOVERY_MODE PRE_TEST
)
