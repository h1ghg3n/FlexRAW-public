if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER)
    add_subdirectory(platform)
endif()

add_subdirectory(core)

add_subdirectory(worker)

if(FLEXRAW_BUILD_DESKTOP)
    add_subdirectory(ui)
    add_subdirectory(app)
endif()
