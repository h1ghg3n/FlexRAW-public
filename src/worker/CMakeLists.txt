add_subdirectory(protocol)
add_subdirectory(runtime)

if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_TESTS)
    add_subdirectory(client)
endif()

if(FLEXRAW_BUILD_WORKER)
    add_subdirectory(admission)
    add_subdirectory(network)
    add_subdirectory(app)

    qt_add_executable(flexraw-worker
        main.cpp
    )

    set_target_properties(flexraw-worker PROPERTIES
        WIN32_EXECUTABLE FALSE
    )

    target_compile_definitions(flexraw-worker PRIVATE
        FLEXRAW_VERSION_STRING="${FLEXRAW_VERSION_STRING}"
    )

    target_link_libraries(flexraw-worker PRIVATE
        flexraw_core_util
        flexraw_worker_app
        Qt6::Core
    )

    if(WIN32)
        qt_generate_deploy_app_script(
            TARGET flexraw-worker
            OUTPUT_SCRIPT flexraw_worker_deploy_script
            NO_TRANSLATIONS
        )
        add_custom_command(TARGET flexraw-worker POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DQT_DEPLOY_PREFIX=$<TARGET_FILE_DIR:flexraw-worker>
                -DQT_DEPLOY_BIN_DIR=.
                -DQT_DEPLOY_LIB_DIR=.
                -DQT_DEPLOY_PLUGINS_DIR=plugins
                -P ${flexraw_worker_deploy_script}
            WORKING_DIRECTORY $<TARGET_FILE_DIR:flexraw-worker>
            COMMENT "Deploying Worker runtime dependencies"
        )

        set(FLEXRAW_WORKER_JPEG_RELEASE_RUNTIME_DLL
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/jpeg62.dll"
        )
        set(FLEXRAW_WORKER_JPEG_DEBUG_RUNTIME_DLL
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin/jpeg62.dll"
        )
        if(NOT EXISTS "${FLEXRAW_WORKER_JPEG_RELEASE_RUNTIME_DLL}")
            message(FATAL_ERROR
                "Required Release Worker JPEG dependency is missing: ${FLEXRAW_WORKER_JPEG_RELEASE_RUNTIME_DLL}"
            )
        endif()
        if(NOT EXISTS "${FLEXRAW_WORKER_JPEG_DEBUG_RUNTIME_DLL}")
            message(FATAL_ERROR
                "Required Debug Worker JPEG dependency is missing: ${FLEXRAW_WORKER_JPEG_DEBUG_RUNTIME_DLL}"
            )
        endif()

        add_custom_command(TARGET flexraw-worker POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<IF:$<CONFIG:Debug>,${FLEXRAW_WORKER_JPEG_DEBUG_RUNTIME_DLL},${FLEXRAW_WORKER_JPEG_RELEASE_RUNTIME_DLL}>"
                $<TARGET_FILE_DIR:flexraw-worker>
            COMMENT "Deploying Worker JPEG plugin dependency"
        )
    endif()
endif()
