# import — FTP 서버, SD카드 감지, 파일 정리

add_library(flexraw_core_import STATIC)

target_sources(flexraw_core_import PRIVATE
    import.cpp
)

target_include_directories(flexraw_core_import PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_import PUBLIC
    flexraw_core_types
    flexraw_core_util
    Qt6::Core
    Qt6::Network
)
