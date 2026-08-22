# worker_network - TCP sessions and bounded render protocol server

add_library(flexraw_worker_network STATIC)

target_sources(flexraw_worker_network PRIVATE
    protocol_session.cpp
    protocol_session.h
    worker_server.cpp
    worker_server.h
)

target_include_directories(flexraw_worker_network PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_worker_network PUBLIC
    flexraw_worker_protocol
    flexraw_worker_runtime
    Qt6::Core
    Qt6::Network
)
