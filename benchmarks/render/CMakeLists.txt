add_executable(flexraw_develop_baseline)

target_sources(flexraw_develop_baseline PRIVATE
    develop_baseline.cpp
)

target_link_libraries(flexraw_develop_baseline PRIVATE
    flexraw_core_develop
    flexraw_core_measurement
    Qt6::Core
    Qt6::Gui
)

add_executable(flexraw_resolved_render_smoke)

target_sources(flexraw_resolved_render_smoke PRIVATE
    resolved_render_smoke.cpp
)

target_link_libraries(flexraw_resolved_render_smoke PRIVATE
    flexraw_core_measurement
    flexraw_core_render
    Qt6::Core
    Qt6::Gui
)

add_executable(flexraw_image_regression_compare)

target_sources(flexraw_image_regression_compare PRIVATE
    image_regression_compare.cpp
)

target_link_libraries(flexraw_image_regression_compare PRIVATE
    flexraw_core_measurement
    Qt6::Core
    Qt6::Gui
)

add_executable(flexraw_image_reference_candidate)

target_sources(flexraw_image_reference_candidate PRIVATE
    image_reference_candidate.cpp
    representative_develop_profile.cpp
    representative_develop_profile.h
)

target_link_libraries(flexraw_image_reference_candidate PRIVATE
    flexraw_core_render
    Qt6::Core
    Qt6::Gui
)
