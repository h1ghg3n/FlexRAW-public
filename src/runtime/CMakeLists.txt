# product runtime — shared Product operation ownership without Adapter dependencies

add_library(flexraw_product_runtime STATIC)

target_sources(flexraw_product_runtime PRIVATE
    product_runtime.cpp
    product_runtime.h
)

target_include_directories(flexraw_product_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_product_runtime PRIVATE
    flexraw_core_orchestration
)
