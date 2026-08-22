# worker_protocol - versioned render worker wire envelope and incremental parser

add_library(flexraw_worker_protocol STATIC)

target_sources(flexraw_worker_protocol PRIVATE
    frame_codec.cpp
    frame_codec.h
    frame_parser.cpp
    frame_parser.h
    job_id.h
    protocol_contracts.h
)

target_include_directories(flexraw_worker_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_worker_protocol PUBLIC
    flexraw_core_types
    Qt6::Core
)

set_target_properties(flexraw_worker_protocol PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
