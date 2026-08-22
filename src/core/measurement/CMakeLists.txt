# measurement - rendering timing and bounded scheduler counters

add_library(flexraw_core_measurement STATIC)

target_sources(flexraw_core_measurement PRIVATE
    high_water_counter.cpp
    high_water_counter.h
    measurement_csv.cpp
    measurement_csv.h
    render_stats.h
    stage_timer.cpp
    stage_timer.h
)

target_include_directories(flexraw_core_measurement PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(flexraw_core_measurement PUBLIC cxx_std_20)

set_target_properties(flexraw_core_measurement PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

if(MSVC)
    target_compile_options(flexraw_core_measurement PRIVATE /utf-8)
endif()
