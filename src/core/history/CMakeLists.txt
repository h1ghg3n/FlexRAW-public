# history — 편집 히스토리 (Ctrl+Z/Y), 사진별 독립 관리

add_library(flexraw_core_history STATIC)

target_sources(flexraw_core_history PRIVATE
    develop_history.cpp
    develop_history.h
)

target_include_directories(flexraw_core_history PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_history PUBLIC
    flexraw_core_types
    flexraw_core_util
    Qt6::Core
)
