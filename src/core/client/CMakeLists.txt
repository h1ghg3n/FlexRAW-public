# client — Qt-free Catalog/Editor public contract values

add_library(flexraw_core_client STATIC)

set_target_properties(flexraw_core_client PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
)

target_sources(flexraw_core_client PRIVATE
    activity_client.h
    catalog_photo_client.h
    catalog_project_client.h
    client_error.h
    client_identity.h
    client_result.h
    display_frame.cpp
    display_frame.h
    editor_client.h
    editor_event_client.h
)

target_include_directories(flexraw_core_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(MSVC)
    target_compile_options(flexraw_core_client PRIVATE /utf-8)
endif()
