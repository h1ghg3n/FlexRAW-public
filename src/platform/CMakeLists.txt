# platform - OS-specific capability adapters selected by the composition root

add_subdirectory(api)

if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER OR FLEXRAW_BUILD_MCP)
    add_subdirectory(current)
endif()

if(WIN32 AND FLEXRAW_BUILD_DESKTOP)
    add_subdirectory(windows_desktop)
endif()
