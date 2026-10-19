# platform_windows_desktop - Qt Windows display integration for the Desktop adapter

add_library(flexraw_platform_windows_desktop STATIC)

target_sources(flexraw_platform_windows_desktop PRIVATE
    windows_display_target_resolver.cpp
    windows_display_target_resolver.h
)

target_include_directories(flexraw_platform_windows_desktop PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(flexraw_platform_windows_desktop PUBLIC
    flexraw_platform_api
    Qt6::Gui
)

target_link_libraries(flexraw_platform_windows_desktop PRIVATE
    User32
)

set_target_properties(flexraw_platform_windows_desktop PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
