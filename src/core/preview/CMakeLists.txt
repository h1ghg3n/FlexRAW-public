# preview — 3단계 프로그레시브 프리뷰

add_library(flexraw_core_preview STATIC)

target_sources(flexraw_core_preview PRIVATE
    preview_cache.cpp
    preview_cache.h
    thumbnail_preview.cpp
    thumbnail_preview.h
)

target_include_directories(flexraw_core_preview PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_preview PUBLIC
    flexraw_core_types
    flexraw_core_util
    flexraw_core_raw
    Qt6::Core
    Qt6::Gui
)
