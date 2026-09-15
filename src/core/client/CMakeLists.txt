# client — Qt-free Catalog/Editor public contract values

add_library(flexraw_core_client STATIC)

set_target_properties(flexraw_core_client PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
)

target_sources(flexraw_core_client PRIVATE
    activity_client.h
    catalog_folder_client.h
    catalog_photo_client.h
    catalog_project_client.h
    catalog_session_client.h
    catalog_startup_settings_client.h
    catalog_thumbnail_client.h
    client_error.h
    client_identity.h
    client_result.h
    display_frame.cpp
    display_frame.h
    editor_client.h
    editor_event_client.h
    export_client.h
    export_defaults_client.h
    folder_import_client.h
    preview_presentation_client.h
    source_resolution_client.h
    worker_health_client.h
    worker_profile_client.h
)

target_include_directories(flexraw_core_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(MSVC)
    target_compile_options(flexraw_core_client PRIVATE /utf-8)
endif()
