# pano — OpenCV cv::Stitcher 파노라마

add_library(flexraw_core_pano STATIC)

target_sources(flexraw_core_pano PRIVATE
    pano.cpp
)

target_include_directories(flexraw_core_pano PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_pano PUBLIC
    flexraw_core_types
    flexraw_core_util
    Qt6::Core
    ${OpenCV_LIBS}
)
