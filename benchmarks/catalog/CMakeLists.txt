add_executable(flexraw_catalog_pagination_benchmark)

target_sources(flexraw_catalog_pagination_benchmark PRIVATE
    catalog_pagination_benchmark.cpp
)

target_link_libraries(flexraw_catalog_pagination_benchmark PRIVATE
    flexraw_core_catalog
    flexraw_platform_current
    Qt6::Core
    Qt6::Sql
)
