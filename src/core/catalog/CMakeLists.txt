# catalog — SQLite 비파괴 편집 DB

add_library(flexraw_core_catalog STATIC)

target_sources(flexraw_core_catalog PRIVATE
    catalog_entry.h
    catalog_database.cpp
    catalog_database.h
    catalog_develop_repository.cpp
    catalog_develop_repository.h
    catalog_preset_repository.h
    catalog_folder_importer.cpp
    catalog_folder_importer.h
    catalog_photo_page.h
    catalog_photo_repository.cpp
    catalog_photo_repository.h
    folder_scanner.cpp
    folder_scanner.h
    source_binding.cpp
    source_binding.h
    source_fingerprint.cpp
    source_fingerprint.h
    source_path.cpp
    source_path.h
)

qt_add_resources(flexraw_core_catalog catalog_migrations
    PREFIX "/sql/migrations"
    BASE "${PROJECT_SOURCE_DIR}/resources/sql/migrations"
    FILES "${PROJECT_SOURCE_DIR}/resources/sql/migrations/v1__init.sql"
          "${PROJECT_SOURCE_DIR}/resources/sql/migrations/v2__create_photos.sql"
          "${PROJECT_SOURCE_DIR}/resources/sql/migrations/v3__create_develop_state.sql"
          "${PROJECT_SOURCE_DIR}/resources/sql/migrations/v4__create_presets.sql"
          "${PROJECT_SOURCE_DIR}/resources/sql/migrations/v5__separate_photo_identity.sql"
          "${PROJECT_SOURCE_DIR}/resources/sql/migrations/v6__add_develop_revision.sql"
          "${PROJECT_SOURCE_DIR}/resources/sql/migrations/v7__add_source_parent_path.sql"
)

target_include_directories(flexraw_core_catalog PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_core_catalog PUBLIC
    flexraw_core_types
    flexraw_core_util
    flexraw_core_preset
    Qt6::Core
    Qt6::Sql
)
