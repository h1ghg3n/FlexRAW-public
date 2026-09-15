# platform - OS-specific capability adapters selected by the composition root

add_subdirectory(api)

if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER)
    add_subdirectory(current)
endif()
