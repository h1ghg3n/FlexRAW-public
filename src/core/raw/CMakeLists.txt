# raw — LibRaw RAW 파싱/디코딩

add_library(flexraw_core_raw STATIC)

target_sources(flexraw_core_raw PRIVATE
    raw_file_reader.cpp
    raw_file_reader.h
)

target_include_directories(flexraw_core_raw PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_raw PUBLIC
    flexraw_core_types
    flexraw_core_util
    Qt6::Core
    Qt6::Gui
    libraw::raw_r
)
