# util — 파일/경로/시간/EXIF 헬퍼 + 로깅 매크로
# types 에만 의존

add_library(flexraw_core_util STATIC)

target_sources(flexraw_core_util PRIVATE
    file_format_registry.cpp
    file_format_registry.h
    log.cpp
    log.h
    supported_extensions.cpp
    supported_extensions.h
)

target_include_directories(flexraw_core_util PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_util PUBLIC
    flexraw_core_types
    Qt6::Core
    spdlog::spdlog
)
