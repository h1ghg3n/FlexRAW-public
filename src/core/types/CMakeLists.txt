# types — 도메인 타입 (Image, Photo, DevelopParams, ColorSpace, Result<T,E> 등)
# 의존성 없는 base 모듈

add_library(flexraw_core_types STATIC)

target_sources(flexraw_core_types PRIVATE
    develop_params.h
    error.h
    file_types.h
    operation_types.h
    photo_identity.h
    result.h
    types.cpp
)

target_include_directories(flexraw_core_types PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_types PUBLIC
    Qt6::Core
)
