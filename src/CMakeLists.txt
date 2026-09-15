if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER OR FLEXRAW_BUILD_MCP)
    add_subdirectory(platform)
endif()

add_subdirectory(core)

if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_MCP)
    add_subdirectory(runtime)
endif()

if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER)
    add_subdirectory(worker)
endif()

if(FLEXRAW_BUILD_DESKTOP)
    add_subdirectory(ui)
    add_subdirectory(app)
endif()

if(FLEXRAW_BUILD_MCP)
    add_subdirectory(app/mcp)
    add_subdirectory(mcp)
endif()
