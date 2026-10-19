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

add_executable(flexraw_portable_processing_tests)

target_sources(flexraw_portable_processing_tests PRIVATE
    color_processing_portable_test.cpp
    develop_processing_portable_test.cpp
    hsl_processing_portable_test.cpp
    export_processing_portable_test.cpp
    owned_rgba8_image_portable_test.cpp
    png_export_portable_test.cpp
    raw_processing_portable_test.cpp
)

target_link_libraries(flexraw_portable_processing_tests PRIVATE
    flexraw_core_color_processing
    flexraw_core_develop_processing
    flexraw_core_export_file
    flexraw_core_export_png
    flexraw_core_export_processing
    flexraw_core_raw_processing
    flexraw_core_render_portable
    GTest::gtest_main
)

if(MSVC)
    target_compile_options(flexraw_portable_processing_tests PRIVATE /utf-8)
endif()

gtest_discover_tests(flexraw_portable_processing_tests
    DISCOVERY_MODE PRE_TEST
)
