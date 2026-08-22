# hdr — OpenCV MergeMertens HDR 합성

add_library(flexraw_core_hdr STATIC)

target_sources(flexraw_core_hdr PRIVATE
    hdr.cpp
)

target_include_directories(flexraw_core_hdr PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_hdr PUBLIC
    flexraw_core_types
    flexraw_core_util
    Qt6::Core
    ${OpenCV_LIBS}
)
