# develop — 보정 파라미터 적용 파이프라인 (노출/WB/톤커브 등)

add_library(flexraw_core_develop STATIC)

target_sources(flexraw_core_develop PRIVATE
    clipping.cpp
    clipping.h
    develop.cpp
    develop.h
    histogram.cpp
    histogram.h
)

target_include_directories(flexraw_core_develop PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_develop PUBLIC
    flexraw_core_types
    flexraw_core_util
    Qt6::Core
    Qt6::Gui
)
