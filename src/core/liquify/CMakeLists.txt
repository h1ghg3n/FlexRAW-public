# liquify — MLS 알고리즘 픽셀 유동화

add_library(flexraw_core_liquify STATIC)

target_sources(flexraw_core_liquify PRIVATE
    liquify.cpp
)

target_include_directories(flexraw_core_liquify PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_liquify PUBLIC
    flexraw_core_types
    flexraw_core_util
    Qt6::Core
    ${OpenCV_LIBS}
)
