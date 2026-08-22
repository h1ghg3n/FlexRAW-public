# import — 임포트 다이얼로그 UI

add_library(flexraw_ui_import STATIC)

target_sources(flexraw_ui_import PRIVATE
    import.cpp
)

target_include_directories(flexraw_ui_import PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_ui_import PUBLIC
    flexraw_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
