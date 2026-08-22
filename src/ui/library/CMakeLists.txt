# library — 라이브러리/그리드 뷰

add_library(flexraw_ui_library STATIC)

target_sources(flexraw_ui_library PRIVATE
    library.cpp
)

target_include_directories(flexraw_ui_library PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_ui_library PUBLIC
    flexraw_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)
